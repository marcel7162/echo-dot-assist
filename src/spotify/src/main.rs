//! hassmic-spotify: the Echo as a Spotify Connect speaker, on librespot.  hassmic (src/hassmic/spotify.c) starts it as
//! its own child, under hassmic's user and with the network group as effective group (lockdown.sh lets that group's
//! sockets reach Spotify; Amazon's daemons stay local), and keeps it running while the setting is on.
//!
//!   hassmic-spotify --name <shown name> --dir <state dir> --port <zeroconf TCP port> --device-id <40 hex>
//!                   [--bitrate 96|160|320] [--normalise 0|1] [--volume 0..100] [--audio-fd 3]
//!
//! Audio: s16le stereo 44.1 kHz on --audio-fd (a pipe hassmic reads into the mixer's music stream), written at the
//! speed the reader takes it; nothing while paused.  The volume is the Echo's: librespot's own attenuation stays at 1,
//! the Spotify app's slider and the Echo's keys meet in hassmic (`volume` both ways).
//!
//! Commands, a line each on stdin:
//!   play                    "Spiel Spotify": resume here, else take over what the account played last anywhere,
//!                           else the liked songs
//!   liked                   the liked songs
//!   search <kind> <words>   kind any|track|artist|album|playlist: the best match plays (src/search.rs)
//!   pause | resume | next | prev
//!   volume <0..100>         the Echo's music volume changed (keys, Home Assistant)
//!   logout                  the stored login goes; the speaker waits for the app again
//!   quit
//! Events, a line each on stdout:
//!   ready                   zeroconf listens: the app can find and log in to the speaker
//!   login <user> | login -  a session with that account is up / none stored (`-` also after logout)
//!   state playing|paused|stopped
//!   track <title>\t<artists>\t<album>\t<cover url>\t<ms>
//!   volume <0..100>         the Spotify app moved the slider
//!   result <cmd> ok <what> | result <cmd> fail nologin|notfound|error   for play, liked and search: what to tell
//! Log lines go to stderr (hassmic's log).

mod search;
mod zeroconf;

use std::fs::File;
use std::io::Write;
use std::os::fd::FromRawFd;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use std::sync::Arc;
use std::time::Duration;

use bytes::Bytes;
use librespot_connect::{ConnectConfig, LoadRequest, LoadRequestOptions, Spirc};
use librespot_core::authentication::Credentials;
use librespot_core::cache::Cache;
use librespot_core::config::{DeviceType, SessionConfig};
use librespot_core::error::ErrorKind;
use librespot_core::session::Session;
use librespot_metadata::audio::UniqueFields;
use librespot_playback::audio_backend::{Sink, SinkError, SinkResult};
use librespot_playback::config::{Bitrate, PlayerConfig};
use librespot_playback::convert::Converter;
use librespot_playback::decoder::AudioPacket;
use librespot_playback::mixer::{Mixer, MixerConfig, NoOpVolume};
use librespot_playback::player::{Player, PlayerEvent};
use tokio::sync::mpsc;

struct Args {
    name: String,
    dir: PathBuf,
    port: u16,
    device_id: String,
    bitrate: Bitrate,
    normalise: bool,
    volume: u16,
    audio_fd: i32,
}

fn args() -> Args {
    let mut a = Args { name: "Echo".into(), dir: PathBuf::from("."), port: 28933, device_id: String::new(), bitrate: Bitrate::Bitrate160, normalise: true, volume: 50, audio_fd: 3 };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let v = it.next().unwrap_or_default();
        match k.as_str() {
            "--name" => a.name = v,
            "--dir" => a.dir = PathBuf::from(v),
            "--port" => a.port = v.parse().unwrap_or(a.port),
            "--device-id" => a.device_id = v,
            "--bitrate" => a.bitrate = match v.as_str() { "96" => Bitrate::Bitrate96, "320" => Bitrate::Bitrate320, _ => Bitrate::Bitrate160 },
            "--normalise" => a.normalise = v != "0",
            "--volume" => a.volume = v.parse::<u16>().unwrap_or(50).min(100),
            "--audio-fd" => a.audio_fd = v.parse().unwrap_or(3),
            _ => eprintln!("spotify: unknown option {k}"),
        }
    }
    if a.device_id.len() != 40 || !a.device_id.bytes().all(|c| c.is_ascii_hexdigit()) {
        // Spotify takes 40 hex digits; hassmic derives them from the Echo's MAC so the app keeps knowing the speaker
        use sha1::Digest;
        a.device_id = sha1::Sha1::digest(a.name.as_bytes()).iter().map(|b| format!("{b:02x}")).collect();
    }
    a
}

/// One line on stdout, whole: hassmic reads them as they come (LineWriter flushes at the newline)
fn emit(line: &str) {
    println!("{}", line.replace('\n', " "));
}

struct Logger;
impl log::Log for Logger {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Info || (m.level() <= log::Level::Debug && m.target().starts_with("hassmic_spotify"))
    }
    fn log(&self, r: &log::Record) {
        if self.enabled(r.metadata()) {
            let t = r.target().split("::").next().unwrap_or("");
            eprintln!("spotify: {} {}: {}", r.level().as_str().to_lowercase(), t, r.args());
        }
    }
    fn flush(&self) {}
}

/// PCM to hassmic: s16le stereo at 44.1 kHz, as librespot decodes it.  A write blocks while the pipe is full, which is
/// what paces the player; a reader gone (hassmic ended) ends this process too
struct PipeSink {
    out: File,
    buf: Vec<u8>,
}

impl Sink for PipeSink {
    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        let AudioPacket::Samples(s) = packet else {
            return Err(SinkError::InvalidParams("raw packets (passthrough) are not decoded".into()));
        };
        self.buf.clear();
        for v in converter.f64_to_s16(&s) {
            self.buf.extend_from_slice(&v.to_le_bytes());
        }
        if let Err(e) = self.out.write_all(&self.buf) {
            log::error!("audio pipe: {e}");
            std::process::exit(0);
        }
        Ok(())
    }
}

/// The Echo's music volume stands in for librespot's mixer: what the app sets goes to hassmic, and nothing is
/// attenuated here (hassmic applies the volume as it does for every source)
struct EchoMixer {
    vol: AtomicU16,
}

fn pct(v: u16) -> u32 {
    (v as u32 * 100 + 32767) / 65535
}

impl Mixer for EchoMixer {
    fn open(_: MixerConfig) -> Result<Self, librespot_core::Error> {
        Ok(EchoMixer { vol: AtomicU16::new(32768) })
    }
    fn volume(&self) -> u16 {
        self.vol.load(Ordering::Relaxed)
    }
    fn set_volume(&self, v: u16) {
        if self.vol.swap(v, Ordering::Relaxed) != v || pct(v) == 0 {
            emit(&format!("volume {}", pct(v)));
        }
    }
}

/// The Echo's clock comes from Home Assistant only (clock.c): before it has, it says 1970, and every certificate of
/// Spotify's is "not valid yet" to rustls, which ends each connection at once with nothing but "Connect" to show for
/// it (2026-11-19, an Echo on a phone's hotspot without Home Assistant).  Before 2026 is no time of ours
fn clock_ok() -> bool {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() >= 1_767_225_600).unwrap_or(false)
}

/// librespot's error and what caused it, all the way down: "Service unavailable { client error (Connect) }" alone
/// hides whether DNS, the connection or TLS failed
fn why(e: &librespot_core::Error) -> String {
    let mut s = e.to_string();
    let mut src = std::error::Error::source(&*e.error);
    while let Some(c) = src {
        s.push_str(": ");
        s.push_str(&c.to_string());
        src = c.source();
    }
    s
}

enum End {
    Quit,
    Logout,
    Credentials(Credentials),
    Lost(librespot_core::Error),
}

#[tokio::main(flavor = "multi_thread", worker_threads = 2)]
async fn main() {
    let _ = log::set_logger(&Logger).map(|_| log::set_max_level(log::LevelFilter::Debug));
    let a = args();
    let audio = unsafe { File::from_raw_fd(a.audio_fd) };
    // ap_port 443: Spotify's access points on 443 only (librespot's apresolve filters by it).  The first choice is
    // 4070, which networks that let only the web's ports out drop: the connection hung in SYN_SENT (2026-11-19)
    let session_config = SessionConfig { device_id: a.device_id.clone(), ap_port: Some(443), ..SessionConfig::default() };
    let cache = match Cache::new(Some(&a.dir), None, None, None) {
        Ok(c) => c,
        Err(e) => {
            log::error!("cannot use {}: {e}", a.dir.display());
            std::process::exit(1);
        }
    };
    let (cred_tx, mut cred_rx) = mpsc::unbounded_channel();
    let zc = zeroconf::Config { name: a.name.clone(), device_id: a.device_id.clone(), client_id: session_config.client_id.clone() };
    if let Err(e) = zeroconf::start(a.port, zc, cred_tx) {
        log::error!("cannot listen on TCP {}: {e}", a.port);
        std::process::exit(1);
    }
    emit("ready");
    // stdin on a thread of its own: its end means hassmic ended or restarted, and the speaker ends with it at once,
    // whatever it is waiting for.  Read only between awaits before, a speaker stuck connecting (an access point on
    // 4070 that a network drops) outlived its hassmic and kept the zeroconf port from the next one (2026-11-19)
    let (line_tx, mut lines) = mpsc::unbounded_channel::<String>();
    std::thread::spawn(move || {
        use std::io::BufRead;
        for l in std::io::stdin().lock().lines() {
            match l {
                Ok(l) => { if line_tx.send(l).is_err() { break; } }
                Err(_) => break,
            }
        }
        log::info!("hassmic is gone: ending");
        std::process::exit(0);
    });
    let mut creds = cache.credentials();
    let audio = Arc::new(std::sync::Mutex::new(Some(audio)));
    let mut volume = a.volume;
    let mut backoff = 5u64;
    loop {
        let Some(c) = creds.take() else {
            emit("login -");
            tokio::select! {
                c = cred_rx.recv() => { creds = c; }
                l = lines.recv() => match l {
                    Some(l) => match l.split(' ').next().unwrap_or("") {
                        "quit" => break,
                        "volume" => volume = l[6..].trim().parse::<u16>().unwrap_or(volume).min(100),
                        c @ ("play" | "liked" | "search") => emit(&format!("result {c} fail nologin")),
                        _ => {}
                    },
                    _ => break,                         // hassmic is gone
                },
            }
            continue;
        };
        if !clock_ok() {
            log::warn!("the Echo's clock is not set (Home Assistant sets it once it is connected): Spotify cannot be reached before");
            emit("error clock");
            tokio::select! {
                n = cred_rx.recv() => { creds = n.or(Some(c)); }
                l = lines.recv() => match l {
                    Some(l) if l != "quit" => {
                        let k = l.split(' ').next().unwrap_or("");
                        if matches!(k, "play" | "liked" | "search") { emit(&format!("result {k} fail clock")); }
                        creds = Some(c);
                    }
                    _ => break,
                },
                _ = tokio::time::sleep(Duration::from_secs(10)) => { creds = Some(c); }
            }
            continue;
        }
        let cache = Cache::new(Some(&a.dir), None, None, None).ok();
        match session(&a, session_config.clone(), cache, c.clone(), &audio, &mut volume, &mut lines, &mut cred_rx).await {
            End::Quit => break,
            End::Logout => {
                let _ = std::fs::remove_file(a.dir.join("credentials.json"));
                log::info!("logged out: the stored login is gone");
            }
            End::Credentials(n) => {
                backoff = 5;
                creds = Some(n);
            }
            End::Lost(e) => {
                emit("state stopped");
                if matches!(e.kind, ErrorKind::Unauthenticated | ErrorKind::PermissionDenied) {
                    log::warn!("login refused ({}): the app has to log in again", why(&e));
                    emit("error refused");
                    let _ = std::fs::remove_file(a.dir.join("credentials.json"));
                    continue;
                }
                log::warn!("session ended ({}); again in {backoff} s", why(&e));
                emit(&format!("error {}", if clock_ok() { "unreachable" } else { "clock" }));
                // a new login from the app meanwhile is taken at once; hassmic's "quit" too (it kills what does not end)
                tokio::select! {
                    c = cred_rx.recv() => { creds = c; backoff = 5; continue; }
                    l = lines.recv() => match l {
                        Some(l) if l != "quit" => {
                            let k = l.split(' ').next().unwrap_or("");
                            if matches!(k, "play" | "liked" | "search") { emit(&format!("result {k} fail error")); }
                        }
                        _ => break,
                    },
                    _ = tokio::time::sleep(Duration::from_secs(backoff)) => {}
                }
                backoff = (backoff * 2).min(300);
                creds = Some(c);
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
async fn session(
    a: &Args,
    config: SessionConfig,
    cache: Option<Cache>,
    creds: Credentials,
    audio: &Arc<std::sync::Mutex<Option<File>>>,
    volume: &mut u16,
    lines: &mut mpsc::UnboundedReceiver<String>,
    cred_rx: &mut mpsc::UnboundedReceiver<Credentials>,
) -> End {
    let session = Session::new(config, cache);
    let pc = PlayerConfig { bitrate: a.bitrate, normalisation: a.normalise, ..PlayerConfig::default() };
    let pipe = audio.clone();
    let player = Player::new(pc, session.clone(), Box::new(NoOpVolume), move || {
        // one player per session, one pipe for the process: the next session's player takes it over
        let out = pipe.lock().ok().and_then(|f| f.as_ref().and_then(|f| f.try_clone().ok()));
        match out {
            Some(out) => Box::new(PipeSink { out, buf: Vec::with_capacity(16384) }) as Box<dyn Sink>,
            None => std::process::exit(1),
        }
    });
    let mixer = Arc::new(EchoMixer { vol: AtomicU16::new((*volume as u32 * 65535 / 100) as u16) });
    let cc = ConnectConfig { name: a.name.clone(), device_type: DeviceType::Speaker, initial_volume: (*volume as u32 * 65535 / 100) as u16, ..ConnectConfig::default() };
    let mut events = player.get_player_event_channel();
    let (spirc, task) = match Spirc::new(cc, session.clone(), creds, player.clone(), mixer).await {
        Ok(s) => s,
        Err(e) => return End::Lost(e),
    };
    let task = tokio::spawn(task);
    emit(&format!("login {}", session.username()));
    let playing = Arc::new(AtomicBool::new(false));
    let active = Arc::new(AtomicBool::new(false));
    let (p2, a2) = (playing.clone(), active.clone());
    let ev = tokio::spawn(async move {
        while let Some(e) = events.recv().await {
            match e {
                PlayerEvent::Playing { .. } => { p2.store(true, Ordering::Relaxed); a2.store(true, Ordering::Relaxed); emit("state playing"); }
                PlayerEvent::Paused { .. } => { p2.store(false, Ordering::Relaxed); emit("state paused"); }
                PlayerEvent::Stopped { .. } => { p2.store(false, Ordering::Relaxed); emit("state stopped"); }
                PlayerEvent::SessionConnected { .. } => a2.store(true, Ordering::Relaxed),
                PlayerEvent::SessionDisconnected { .. } => { a2.store(false, Ordering::Relaxed); p2.store(false, Ordering::Relaxed); emit("state stopped"); }
                PlayerEvent::TrackChanged { audio_item } => {
                    let (artists, album) = match &audio_item.unique_fields {
                        UniqueFields::Track { artists, album, .. } => (artists.iter().map(|x| x.name.as_str()).collect::<Vec<_>>().join(", "), album.clone()),
                        UniqueFields::Episode { show_name, .. } => (show_name.clone(), String::new()),
                        UniqueFields::Local { artists, album, .. } => (artists.clone().unwrap_or_default(), album.clone().unwrap_or_default()),
                    };
                    let cover = audio_item.covers.iter().max_by_key(|c| c.width).map(|c| c.url.clone()).unwrap_or_default();
                    let clean = |s: &str| s.replace(['\t', '\n'], " ");
                    emit(&format!("track {}\t{}\t{}\t{}\t{}", clean(&audio_item.name), clean(&artists), clean(&album), cover, audio_item.duration_ms));
                }
                _ => {}
            }
        }
    });
    let end = loop {
        tokio::select! {
            c = cred_rx.recv() => match c {
                // the app logged in again (maybe another account): a new session with it
                Some(c) => { let _ = spirc.shutdown(); break End::Credentials(c); }
                None => continue,
            },
            l = lines.recv() => {
                let l = match l { Some(l) => l, _ => { let _ = spirc.shutdown(); break End::Quit; } };
                let (cmd, rest) = l.split_once(' ').unwrap_or((l.as_str(), ""));
                match cmd {
                    "quit" => { let _ = spirc.shutdown(); break End::Quit; }
                    "logout" => { let _ = spirc.shutdown(); break End::Logout; }
                    "pause" => { let _ = spirc.pause(); }
                    "resume" => { let _ = spirc.play(); }
                    "next" => { let _ = spirc.next(); }
                    "prev" => { let _ = spirc.prev(); }
                    "volume" => if let Ok(v) = rest.trim().parse::<u16>() {
                        *volume = v.min(100);
                        let raw = (*volume as u32 * 65535 / 100) as u16;
                        if active.load(Ordering::Relaxed) { let _ = spirc.set_volume(raw); }
                    },
                    "play" => play(&session, &spirc, &playing, &active).await,
                    "liked" => liked(&session, &spirc, "liked").await,
                    "search" => {
                        let (kind, words) = rest.split_once(' ').unwrap_or(("any", rest));
                        find(&session, &spirc, search::Kind::parse(kind).unwrap_or(search::Kind::Any), words.trim()).await;
                    }
                    _ => log::warn!("unknown command {cmd}"),
                }
            }
            _ = wait_done(&task) => {
                break End::Lost(librespot_core::Error::unavailable("the Connect task ended"));
            }
        }
    };
    let _ = tokio::time::timeout(Duration::from_secs(3), async { while !task.is_finished() { tokio::time::sleep(Duration::from_millis(50)).await; } }).await;
    task.abort();
    ev.abort();
    session.shutdown();
    end
}

async fn wait_done(t: &tokio::task::JoinHandle<()>) {
    while !t.is_finished() {
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
}

fn start() -> LoadRequestOptions {
    LoadRequestOptions { start_playing: true, ..LoadRequestOptions::default() }
}

/// "Spiel Spotify".  Paused here: on.  Otherwise what the account played last, wherever (Connect's transfer to this
/// device, as picking it in the app does); an account that played nothing anywhere has nothing to take over, so after
/// 5 s without playback here the liked songs start
async fn play(session: &Session, spirc: &Spirc, playing: &AtomicBool, active: &AtomicBool) {
    if playing.load(Ordering::Relaxed) {
        emit("result play ok playing");
        return;
    }
    if active.load(Ordering::Relaxed) {
        let _ = spirc.play();
    } else {
        let _ = spirc.transfer(None);
    }
    for _ in 0..50 {
        tokio::time::sleep(Duration::from_millis(100)).await;
        if playing.load(Ordering::Relaxed) {
            emit("result play ok resumed");
            return;
        }
    }
    log::info!("nothing to resume: the liked songs");
    liked(session, spirc, "play").await;
}

async fn liked(session: &Session, spirc: &Spirc, cmd: &str) {
    let _ = spirc.activate();
    let uri = format!("spotify:user:{}:collection", session.username());
    match spirc.load(LoadRequest::from_context_uri(uri, start())) {
        Ok(()) => emit(&format!("result {cmd} ok liked")),
        Err(e) => {
            log::warn!("liked songs: {e}");
            emit(&format!("result {cmd} fail error"));
        }
    }
}

async fn find(session: &Session, spirc: &Spirc, kind: search::Kind, words: &str) {
    if words.is_empty() {
        emit("result search fail notfound");
        return;
    }
    let body = tokio::time::timeout(Duration::from_secs(10), async {
        let token = session.login5().auth_token().await?;
        let req = http::Request::get(search::url(kind, words))
            .header("Authorization", format!("Bearer {}", token.access_token))
            .body(Bytes::new())?;
        session.http_client().request_body(req).await
    })
    .await;
    let v: serde_json::Value = match body {
        Ok(Ok(b)) => serde_json::from_slice(&b).unwrap_or_default(),
        Ok(Err(e)) => {
            log::warn!("search \"{words}\": {e}");
            emit("result search fail error");
            return;
        }
        Err(_) => {
            log::warn!("search \"{words}\": no answer in 10 s");
            emit("result search fail error");
            return;
        }
    };
    let Some(c) = search::pick(&v, kind, words) else {
        log::info!("search \"{words}\" ({}): nothing", kind.name());
        emit("result search fail notfound");
        return;
    };
    log::info!("search \"{words}\" ({}): {} {}", kind.name(), c.kind.name(), c.uri);
    let _ = spirc.activate();
    let req = if c.kind == search::Kind::Track { LoadRequest::from_tracks(vec![c.uri.clone()], start()) } else { LoadRequest::from_context_uri(c.uri.clone(), start()) };
    match spirc.load(req) {
        Ok(()) => emit(&format!("result search ok {} {}", c.kind.name(), c.name.replace('\n', " "))),
        Err(e) => {
            log::warn!("load {}: {e}", c.uri);
            emit("result search fail error");
        }
    }
}
