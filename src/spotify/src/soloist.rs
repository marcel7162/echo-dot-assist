//! `--engine soloist`: the same lines to and from hassmic as librespot's speaker (main.rs lists them), but the music
//! from Spotify Soloist (src/soloist/README.md), Spotify's own Connect client: librespot logs in, but Spotify refuses
//! some accounts every track's key (librespot issue #1649), Soloist gets them.
//!
//! Soloist's arm32 build is downloaded from Spotify here (each Echo its own: Spotify does not allow passing it on) and
//! fetched again after 30 days (a build stops 90 days after it was made).  It runs under glibc's loader with the
//! runtime the bundle brings next to hassmic (--runtime): libpulse.so.0 (src/soloist/hmpulse.c) preloaded, its PCM to
//! hassmic's pipe (HMPULSE_FD, the audio fd this process got), the volume Soloist sets reported, not applied
//! (HMPULSE_VOLUME=report: hassmic owns the Echo's volume) on a pipe of ours, its zeroconf listener on --zc-port.
//! Soloist announces itself (its own mDNS; it worked beside avahi on the Echo) and turns discovery off once logged in.
//! What it does comes from `soloist ctl trace` (JSON lines: auth_state, playback_state, playback_changed); commands
//! are `soloist ctl <command>` against its WebSocket API on 127.0.0.1:--ws-port.

use std::io::{BufRead, BufReader, Read};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime};

use bytes::Bytes;
use serde_json::Value;
use tokio::sync::mpsc;

use crate::emit;

const URL: &str = "https://soloist-builds.spotifycdn.com/soloist_release_arm32.tar.gz";
const REFRESH: Duration = Duration::from_secs(30 * 24 * 3600);

pub struct Opts {
    pub runtime: PathBuf,
    pub dir: PathBuf,
    pub key: String,
    pub name: String,
    pub volume: u16,
    pub ws_port: u16,
    pub zc_port: u16,
    pub audio_fd: i32,
    pub webapi: Option<(String, String)>,   // the owner's Web API app (client id, secret): searches; Soloist has none
}

/// `--webapi-file`: client id and secret, a line each (hassmic's state/spotify/webapi, 0600)
pub fn webapi_read(p: &Path) -> Option<(String, String)> {
    let t = std::fs::read_to_string(p).ok()?;
    let mut l = t.lines().map(str::trim).filter(|l| !l.is_empty());
    let (id, secret) = (l.next()?.to_string(), l.next()?.to_string());
    Some((id, secret))
}

/// The app's token (client credentials), kept until a minute before it ends
static TOKEN: Mutex<Option<(String, std::time::Instant)>> = Mutex::new(None);

async fn app_token(id: &str, secret: &str) -> Result<String, String> {
    if let Some((t, end)) = TOKEN.lock().unwrap().clone() {
        if std::time::Instant::now() < end { return Ok(t); }
    }
    use base64::Engine;
    let basic = base64::engine::general_purpose::STANDARD.encode(format!("{id}:{secret}"));
    let req = http::Request::post("https://accounts.spotify.com/api/token")
        .header("Authorization", format!("Basic {basic}"))
        .header("Content-Type", "application/x-www-form-urlencoded")
        .body(Bytes::from_static(b"grant_type=client_credentials"))
        .map_err(|e| e.to_string())?;
    let body = http_body(req).await?;
    let v: Value = serde_json::from_slice(&body).map_err(|_| "not JSON".to_string())?;
    let Some(t) = v.get("access_token").and_then(Value::as_str) else {
        return Err(format!("no token: {}", v.get("error").and_then(Value::as_str).unwrap_or("?")));
    };
    let secs = v.get("expires_in").and_then(Value::as_u64).unwrap_or(3600).saturating_sub(60);
    *TOKEN.lock().unwrap() = Some((t.to_string(), std::time::Instant::now() + Duration::from_secs(secs)));
    Ok(t.to_string())
}

async fn http_body(req: http::Request<Bytes>) -> Result<Bytes, String> {
    let client = librespot_core::http_client::HttpClient::new(None);
    tokio::time::timeout(Duration::from_secs(10), client.request_body(req)).await.map_err(|_| "no answer in 10 s".to_string())?.map_err(|e| e.to_string())
}

/// "search <kind> <words>": the app's token, Web API's search, the best match (search.rs), then Soloist plays it
async fn find(o: Arc<Opts>, sh: Arc<Shared>, rest: String) {
    let Some((id, secret)) = o.webapi.clone() else { emit("result search fail nosearch"); return };
    if !sh.logged_in.load(Ordering::Relaxed) { emit("result search fail nologin"); return; }
    let (kind, words) = rest.split_once(' ').unwrap_or(("any", rest.as_str()));
    let (kind, words) = (crate::search::Kind::parse(kind).unwrap_or(crate::search::Kind::Any), words.trim().to_string());
    if words.is_empty() { emit("result search fail notfound"); return; }
    let mut v = None;
    for _ in 0..2 {
        let tok = match app_token(&id, &secret).await {
            Ok(t) => t,
            Err(e) => { log::warn!("Web API token: {e}"); emit("result search fail webapi"); return; }
        };
        let req = http::Request::get(crate::search::url_app(kind, &words)).header("Authorization", format!("Bearer {tok}")).body(Bytes::new()).unwrap();
        match http_body(req).await {
            Ok(b) => { v = serde_json::from_slice::<Value>(&b).ok(); break; }
            // a token Spotify ended early: once more with a new one
            Err(e) => { log::warn!("search \"{words}\": {e}"); *TOKEN.lock().unwrap() = None; }
        }
    }
    let Some(v) = v else { emit("result search fail error"); return };
    let Some(c) = crate::search::pick(&v, kind, &words) else {
        log::info!("search \"{words}\" ({}): nothing", kind.name());
        emit("result search fail notfound");
        return;
    };
    log::info!("search \"{words}\" ({}): {} {}", kind.name(), c.kind.name(), c.uri);
    let ok = tokio::task::spawn_blocking(move || { ctl(&o, &["activate"]); ctl(&o, &["play", &c.uri]) }).await.unwrap_or(false);
    if ok { emit(&format!("result search ok {} {}", c.kind.name(), c.name.replace('\n', " "))) } else { emit("result search fail error") }
}

/// Soloist's pid, for the stdin thread to end it with us: left running it would hold its ports from the next start
pub static CHILD: AtomicI32 = AtomicI32::new(0);

pub fn end_child() {
    let p = CHILD.swap(0, Ordering::SeqCst);
    if p > 0 {
        unsafe { libc::kill(p, libc::SIGTERM) };
    }
}

fn bin(o: &Opts) -> PathBuf {
    o.dir.join("bin").join("soloist")
}

fn stale(p: &Path) -> bool {
    match std::fs::metadata(p).and_then(|m| m.modified()) {
        Ok(t) => SystemTime::now().duration_since(t).map(|d| d > REFRESH).unwrap_or(false),
        Err(_) => true,
    }
}

/// The "soloist" member of Spotify's tar.gz: 512-byte headers, the name in the first 100 bytes, the size in octal
pub fn untar_soloist(gz: &[u8]) -> Option<Vec<u8>> {
    let mut tar = Vec::new();
    flate2::read::GzDecoder::new(gz).read_to_end(&mut tar).ok()?;
    let mut at = 0;
    while at + 512 <= tar.len() {
        let h = &tar[at..at + 512];
        if h.iter().all(|&b| b == 0) {
            break;
        }
        let name = String::from_utf8_lossy(&h[..100]).trim_end_matches('\0').to_string();
        let size = usize::from_str_radix(String::from_utf8_lossy(&h[124..136]).trim_matches(|c: char| c == '\0' || c == ' '), 8).ok()?;
        let body = at + 512;
        if body + size > tar.len() {
            return None;
        }
        if name == "soloist" || name.ends_with("/soloist") || name == "./soloist" {
            return Some(tar[body..body + size].to_vec());
        }
        at = body + size.div_ceil(512) * 512;
    }
    None
}

async fn download(o: &Opts) -> Result<(), String> {
    let client = librespot_core::http_client::HttpClient::new(None);
    let req = http::Request::get(URL).body(Bytes::new()).map_err(|e| e.to_string())?;
    let gz = tokio::time::timeout(Duration::from_secs(300), client.request_body(req))
        .await
        .map_err(|_| "no answer in 5 min".to_string())?
        .map_err(|e| e.to_string())?;
    let elf = untar_soloist(&gz).ok_or("no soloist in Spotify's archive")?;
    if elf.len() < 4 || &elf[..4] != b"\x7fELF" {
        return Err("Spotify's archive held no program".into());
    }
    let dir = o.dir.join("bin");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let tmp = dir.join("soloist.new");
    std::fs::write(&tmp, &elf).map_err(|e| e.to_string())?;
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o755)).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, bin(o)).map_err(|e| e.to_string())?;
    log::info!("Soloist downloaded from Spotify ({} MB)", elf.len() / 1_000_000);
    Ok(())
}

/// The loader with Soloist and the runtime: `<rt>/ld-linux-armhf.so.3 --library-path <rt> [--preload ..] <soloist> args`
fn soloist_cmd(o: &Opts, preload: bool) -> Command {
    let mut c = Command::new(o.runtime.join("ld-linux-armhf.so.3"));
    c.arg("--library-path").arg(&o.runtime);
    if preload {
        c.arg("--preload").arg(o.runtime.join("libpulse.so.0"));
    }
    c.arg(bin(o));
    c.env("HOME", o.dir.join("data"));
    c
}

fn ctl(o: &Opts, args: &[&str]) -> bool {
    let ws = format!("127.0.0.1:{}", o.ws_port);
    let mut c = soloist_cmd(o, false);
    c.args(["ctl", "-w", &ws]).args(args).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::piped());
    match c.output() {
        Ok(out) if out.status.success() => true,
        Ok(out) => {
            log::warn!("soloist ctl {}: {}", args.join(" "), String::from_utf8_lossy(&out.stderr).trim());
            false
        }
        Err(e) => {
            log::warn!("soloist ctl {}: {e}", args.join(" "));
            false
        }
    }
}

/// What Soloist says in its log that matters here; the rest (it is chatty: hundreds of lines a track) stays out of
/// the Echo's log, which is on flash
fn keep_log(l: &str) -> bool {
    l.contains(" E [") || l.contains(" W [") || l.contains("logged in as") || l.contains("waiting for login") || l.contains("client expires")
        || l.contains("] playing spotify:") || l.contains("hmpulse:")
        || l.contains("ready, device_name=")     // its device id: one per Echo (main.sh soloist_prep's machine-id)
}

struct Shared {
    user: Mutex<String>,
    playing: AtomicBool,
    logged_in: AtomicBool,
}

/// One `ctl trace` line: "<ms> <json>"
pub fn trace_event(l: &str) -> Vec<String> {
    let json = l.split_once(' ').map(|(_, j)| j).unwrap_or(l);
    let Ok(v) = serde_json::from_str::<Value>(json) else { return vec![] };
    let s = |p: &[&str]| -> String {
        let mut x = &v;
        for k in p {
            x = match x.get(*k) { Some(y) => y, None => return String::new() };
        }
        x.as_str().unwrap_or("").to_string()
    };
    match v.get("type").and_then(Value::as_str).unwrap_or("") {
        "auth_state" => vec![],                         // the user's name comes from the log
        "playback_changed" => match s(&["status"]).as_str() {
            "playing" => vec!["state playing".into()],
            "paused" => vec!["state paused".into()],
            "" => vec![],
            _ => vec!["state stopped".into()],
        },
        "playback_state" => {
            let mut out = vec![format!("state {}", match s(&["status"]).as_str() { "playing" => "playing", "paused" => "paused", _ => "stopped" })];
            let item = v.get("item");
            if let Some(item) = item {
                let name = |x: &Value| x.pointer("/decorations/identity/name").and_then(Value::as_str).unwrap_or("").replace(['\t', '\n'], " ");
                let title = name(item);
                let artists = item.get("creators").and_then(Value::as_array).map(|a| a.iter().filter_map(|c| c.get("entity")).map(name).collect::<Vec<_>>().join(", ")).unwrap_or_default();
                let album = item.pointer("/parent/entity").map(name).unwrap_or_default();
                let cover = item.pointer("/decorations/visual_identity/cover").and_then(Value::as_array)
                    .and_then(|c| c.iter().find(|x| x.get("size").and_then(Value::as_str) == Some("large")).or(c.last()))
                    .and_then(|x| x.get("url")).and_then(Value::as_str).unwrap_or("").to_string();
                let ms = item.pointer("/playback/duration_ms").and_then(Value::as_u64).unwrap_or(0);
                if !title.is_empty() {
                    out.push(format!("track {title}\t{artists}\t{album}\t{cover}\t{ms}"));
                }
            }
            out
        }
        _ => vec![],
    }
}

fn spawn_soloist(o: &Opts, sh: &Arc<Shared>) -> Result<Child, String> {
    let (ev_r, ev_w) = {
        let mut fds = [0; 2];
        if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
            return Err("no pipe".into());
        }
        unsafe { (OwnedFd::from_raw_fd(fds[0]), OwnedFd::from_raw_fd(fds[1])) }
    };
    let ev_w_raw = ev_w.as_raw_fd();
    let audio = o.audio_fd;
    let ws = format!("127.0.0.1:{}", o.ws_port);
    // Soloist seals its stored login with its device id (/etc/machine-id): one kept under another id ("Empty
    // device_id; refusing to decrypt") left it neither logged in nor waiting for the app.  A new id: a fresh start
    let mid = std::fs::read_to_string("/etc/machine-id").unwrap_or_default().trim().to_string();
    let seen = o.dir.join("machine-id");
    if std::fs::read_to_string(&seen).unwrap_or_default().trim() != mid {
        if o.dir.join("data").exists() {
            log::info!("Soloist: a new device id, its stored login forgotten (pick the Echo in the app again)");
        }
        let _ = std::fs::remove_dir_all(o.dir.join("data"));
        let _ = std::fs::write(&seen, &mid);
    }
    for d in ["data", "cache"] {
        let _ = std::fs::create_dir_all(o.dir.join(d));
    }
    let mut c = soloist_cmd(o, true);
    c.args(["-n", &o.name, "-k", &o.key]).arg("-D").arg(o.dir.join("data")).arg("-C").arg(o.dir.join("cache"))
        .args(["-z", "200", "-i", &o.volume.to_string(), "-w", &ws])
        .env("HMPULSE_FD", audio.to_string()).env("HMPULSE_VOLUME", "report").env("HMPULSE_EVENTS_FD", ev_w_raw.to_string())
        .env("HMPULSE_ZC_PORT", o.zc_port.to_string())
        .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    // the two descriptors Soloist's libpulse writes to: kept open across the exec (Rust opens its own with CLOEXEC)
    unsafe {
        c.pre_exec(move || {
            for fd in [audio, ev_w_raw] {
                let f = libc::fcntl(fd, libc::F_GETFD);
                if f >= 0 { libc::fcntl(fd, libc::F_SETFD, f & !libc::FD_CLOEXEC); }
            }
            Ok(())
        });
    }
    let mut child = c.spawn().map_err(|e| format!("cannot start Soloist: {e}"))?;
    drop(ev_w);
    CHILD.store(child.id() as i32, Ordering::SeqCst);
    // its log: what matters, and who is logged in
    for out in [child.stdout.take().map(|s| Box::new(s) as Box<dyn Read + Send>), child.stderr.take().map(|s| Box::new(s) as Box<dyn Read + Send>)].into_iter().flatten() {
        let sh = sh.clone();
        std::thread::spawn(move || {
            for l in BufReader::new(out).lines().map_while(Result::ok) {
                if let Some((_, u)) = l.split_once("logged in as ") {
                    let u = u.trim().to_string();
                    *sh.user.lock().unwrap() = u.clone();
                    sh.logged_in.store(true, Ordering::Relaxed);
                    emit(&format!("login {u}"));
                } else if l.contains("waiting for login") {
                    sh.logged_in.store(false, Ordering::Relaxed);
                    emit("login -");
                }
                if keep_log(&l) {
                    eprintln!("soloist: {}", l.trim());
                }
            }
        });
    }
    // the volume Soloist set (the app's slider): "volume <0..100>" lines from libpulse
    std::thread::spawn(move || {
        for l in BufReader::new(std::fs::File::from(ev_r)).lines().map_while(Result::ok) {
            if l.starts_with("volume ") {
                emit(&l);
            }
        }
    });
    Ok(child)
}

/// `ctl trace` for as long as Soloist runs: started again when it ends (Soloist's API not up yet, or restarted)
fn trace_loop(o: Arc<Opts>, sh: Arc<Shared>, alive: Arc<AtomicBool>) {
    std::thread::spawn(move || {
        while alive.load(Ordering::Relaxed) {
            std::thread::sleep(Duration::from_secs(2));
            let ws = format!("127.0.0.1:{}", o.ws_port);
            let mut c = soloist_cmd(&o, false);
            c.args(["ctl", "-w", &ws, "trace"]).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null());
            let Ok(mut t) = c.spawn() else { continue };
            if let Some(out) = t.stdout.take() {
                for l in BufReader::new(out).lines().map_while(Result::ok) {
                    if !alive.load(Ordering::Relaxed) {
                        break;
                    }
                    for e in trace_event(&l) {
                        if e.starts_with("state ") {
                            sh.playing.store(e == "state playing", Ordering::Relaxed);
                        }
                        emit(&e);
                    }
                }
            }
            let _ = t.kill();
            let _ = t.wait();
        }
    });
}

pub async fn run(o: Opts, lines: &mut mpsc::UnboundedReceiver<String>) {
    let o = Arc::new(o);
    emit("ready");
    let mut backoff = 5u64;
    loop {
        // Spotify's servers need a clock (main.rs clock_ok), and the program from Spotify
        if !crate::clock_ok() {
            emit("error clock");
            if wait_or_quit(lines, 10).await { return; }
            continue;
        }
        if o.key.is_empty() {
            emit("error nokey");
            emit("login -");
            if wait_or_quit(lines, 3600).await { return; }
            continue;
        }
        if stale(&bin(&o)) {
            if let Err(e) = download(&o).await {
                log::warn!("Soloist from Spotify: {e}");
                if !bin(&o).exists() {
                    emit("error download");
                    if wait_or_quit(lines, backoff).await { return; }
                    backoff = (backoff * 2).min(600);
                    continue;
                }
            }
        }
        let sh = Arc::new(Shared { user: Mutex::new(String::new()), playing: AtomicBool::new(false), logged_in: AtomicBool::new(false) });
        let mut child = match spawn_soloist(&o, &sh) {
            Ok(c) => c,
            Err(e) => {
                log::warn!("{e}");
                emit("error start");
                if wait_or_quit(lines, backoff).await { return; }
                backoff = (backoff * 2).min(300);
                continue;
            }
        };
        let started = std::time::Instant::now();
        let alive = Arc::new(AtomicBool::new(true));
        trace_loop(o.clone(), sh.clone(), alive.clone());
        let quit = loop {
            tokio::select! {
                l = lines.recv() => {
                    let Some(l) = l else { break true };
                    let (cmd, rest) = l.split_once(' ').unwrap_or((l.as_str(), ""));
                    let o2 = o.clone(); let sh2 = sh.clone();
                    match cmd {
                        "quit" => break true,
                        "logout" => {
                            // Soloist keeps its session in its data folder: gone, and Soloist waits for the app again
                            let _ = child.kill(); let _ = child.wait();
                            let _ = std::fs::remove_dir_all(o.dir.join("data"));
                            emit("login -");
                            break false;
                        }
                        "volume" | "pause" | "resume" | "next" | "prev" => {
                            let args: Vec<String> = match cmd {
                                "volume" => vec!["volume".into(), rest.trim().to_string()],
                                "resume" => vec!["play".into()],
                                c => vec![c.to_string()],
                            };
                            tokio::task::spawn_blocking(move || { let a: Vec<&str> = args.iter().map(String::as_str).collect(); ctl(&o2, &a); });
                        }
                        "play" => {
                            tokio::task::spawn_blocking(move || {
                                if !sh2.logged_in.load(Ordering::Relaxed) { emit("result play fail nologin"); return; }
                                // "Spiel Spotify": this Echo the active device, then on with what was playing
                                ctl(&o2, &["activate"]);
                                if ctl(&o2, &["play"]) { emit("result play ok resumed") } else { emit("result play fail error") }
                            });
                        }
                        "liked" => {
                            tokio::task::spawn_blocking(move || {
                                let u = sh2.user.lock().unwrap().clone();
                                if u.is_empty() { emit("result liked fail nologin"); return; }
                                ctl(&o2, &["activate"]);
                                let uri = format!("spotify:user:{u}:collection");
                                if ctl(&o2, &["play", &uri]) { emit("result liked ok liked") } else { emit("result liked fail error") }
                            });
                        }
                        "search" => { tokio::spawn(find(o2, sh2, rest.to_string())); }
                        _ => log::warn!("unknown command {cmd}"),
                    }
                }
                _ = tokio::time::sleep(Duration::from_millis(500)) => {
                    if let Ok(Some(st)) = child.try_wait() {
                        log::warn!("Soloist ended ({st})");
                        break false;
                    }
                }
            }
        };
        alive.store(false, Ordering::Relaxed);
        let _ = child.kill();
        let _ = child.wait();
        CHILD.store(0, Ordering::SeqCst);
        emit("state stopped");
        if quit {
            return;
        }
        if started.elapsed() < Duration::from_secs(60) {
            emit("error ended");
            if wait_or_quit(lines, backoff).await { return; }
            backoff = (backoff * 2).min(300);
        } else {
            backoff = 5;
        }
    }
}

/// Waits up to `secs`, answering commands as refused meanwhile; true: hassmic said quit (or is gone)
async fn wait_or_quit(lines: &mut mpsc::UnboundedReceiver<String>, secs: u64) -> bool {
    let end = tokio::time::Instant::now() + Duration::from_secs(secs);
    loop {
        tokio::select! {
            l = lines.recv() => match l {
                None => return true,
                Some(l) if l == "quit" => return true,
                Some(l) => {
                    let k = l.split(' ').next().unwrap_or("");
                    if matches!(k, "play" | "liked" | "search") { emit(&format!("result {k} fail error")); }
                }
            },
            _ = tokio::time::sleep_until(end) => return false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn trace_lines_become_hassmics() {
        // as `soloist ctl trace` printed them on the tester's PC (2026-11-19), shortened
        let l = r#"1795085270488 {"type":"playback_state","status":"playing","item":{"uri":"spotify:track:6Gq","entity_type":"track","decorations":{"identity":{"name":"A Little Bit Off"},"visual_identity":{"cover":[{"url":"https://i.scdn.co/s","size":"small"},{"url":"https://i.scdn.co/l","size":"large"}]}},"parent":{"entity":{"uri":"spotify:album:7v","decorations":{"identity":{"name":"F8"}}}},"creators":[{"entity":{"uri":"spotify:artist:5t","decorations":{"identity":{"name":"Five Finger Death Punch"}}}}],"playback":{"duration_ms":190718}},"volume":40,"is_active":true}"#;
        assert_eq!(trace_event(l), vec!["state playing".to_string(), "track A Little Bit Off\tFive Finger Death Punch\tF8\thttps://i.scdn.co/l\t190718".to_string()]);
        assert_eq!(trace_event(r#"1795085278772 {"type":"playback_changed","status":"paused"}"#), vec!["state paused".to_string()]);
        assert!(trace_event(r#"1795085270487 {"type":"auth_state","logged_in":true}"#).is_empty());
        assert!(trace_event("connected to ws://127.0.0.1:28937").is_empty());
    }

    #[test]
    fn the_owners_app_for_searches() {
        let d = std::env::temp_dir().join(format!("webapi-{}", std::process::id()));
        std::fs::write(&d, "abc123\n  s3cr3t \n").unwrap();
        assert_eq!(webapi_read(&d), Some(("abc123".into(), "s3cr3t".into())));
        std::fs::write(&d, "abc123\n").unwrap();
        assert_eq!(webapi_read(&d), None);
        std::fs::remove_file(&d).unwrap();
        assert!(!crate::search::url_app(crate::search::Kind::Any, "Queen").contains("market"));
    }

    #[test]
    fn soloist_out_of_spotifys_archive() {
        // a tar.gz as Spotify's: CHANGELOG.md, then soloist
        let mut tar = Vec::new();
        for (name, body) in [("CHANGELOG.md", &b"# Soloist"[..]), ("soloist", &b"\x7fELF and the rest"[..])] {
            let mut h = [0u8; 512];
            h[..name.len()].copy_from_slice(name.as_bytes());
            let size = format!("{:011o}\0", body.len());
            h[124..136].copy_from_slice(size.as_bytes());
            tar.extend_from_slice(&h);
            tar.extend_from_slice(body);
            tar.resize(tar.len().div_ceil(512) * 512, 0);
        }
        tar.extend_from_slice(&[0u8; 1024]);
        let mut gz = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::default());
        std::io::Write::write_all(&mut gz, &tar).unwrap();
        assert_eq!(untar_soloist(&gz.finish().unwrap()).as_deref(), Some(&b"\x7fELF and the rest"[..]));
        assert!(untar_soloist(b"not gzip").is_none());
    }
}
