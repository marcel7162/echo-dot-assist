//! Spotify Connect's zeroconf login (getInfo / addUser), served on a fixed port in the firewall's inbound range
//! (lockdown.sh: TCP 16384-32767).  The protocol and its crypto as librespot-discovery 0.8.0's server.rs (MIT), which
//! is not public on its own: its crate always starts an mDNS responder of its own, and on the Echo Amazon's
//! avahi-daemon already holds UDP 5353 and announces our services from a file (main.sh avahi_start, spotify.c).
//! A plain thread per connection: the Spotify app asks twice per login, the requests fit in one read.

use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use aes::cipher::{KeyIvInit, StreamCipher};
use base64::engine::general_purpose::STANDARD as BASE64;
use base64::Engine as _;
use hmac::{Hmac, Mac};
use librespot_core::authentication::Credentials;
use librespot_core::diffie_hellman::DhLocalKeys;
use serde_json::json;
use sha1::{Digest, Sha1};
use tokio::sync::mpsc::UnboundedSender;

type Aes128Ctr = ctr::Ctr128BE<aes::Aes128>;

pub struct Config {
    pub name: String,
    pub device_id: String,
    pub client_id: String,
}

struct State {
    config: Config,
    keys: DhLocalKeys,
    user: Mutex<String>,
    tx: UnboundedSender<Credentials>,
}

/// Binds the port and serves it from a thread of its own; credentials a phone hands over go to `tx`
pub fn start(port: u16, config: Config, tx: UnboundedSender<Credentials>) -> std::io::Result<()> {
    let listener = TcpListener::bind(("0.0.0.0", port))?;
    let st = Arc::new(State { config, keys: DhLocalKeys::random(&mut rand::rng()), user: Mutex::new(String::new()), tx });
    std::thread::spawn(move || {
        for conn in listener.incoming().flatten() {
            let st = st.clone();
            std::thread::spawn(move || {
                if let Err(e) = serve(conn, &st) {
                    log::debug!("zeroconf: {e}");
                }
            });
        }
    });
    Ok(())
}

fn serve(mut s: TcpStream, st: &State) -> std::io::Result<()> {
    s.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut buf = Vec::with_capacity(4096);
    let mut chunk = [0u8; 4096];
    // the head, then as much body as Content-Length says; 64 KiB at most (addUser's blob is under 1 KiB)
    let (head_end, body_len) = loop {
        let n = s.read(&mut chunk)?;
        if n == 0 {
            return Ok(());
        }
        buf.extend_from_slice(&chunk[..n]);
        if buf.len() > 65536 {
            return respond(&mut s, 413, "");
        }
        if let Some(i) = find(&buf, b"\r\n\r\n") {
            let head = String::from_utf8_lossy(&buf[..i]).to_string();
            let len = head
                .lines()
                .filter_map(|l| l.split_once(':'))
                .find(|(k, _)| k.trim().eq_ignore_ascii_case("content-length"))
                .and_then(|(_, v)| v.trim().parse::<usize>().ok())
                .unwrap_or(0);
            if len > 65536 {
                return respond(&mut s, 413, "");
            }
            break (i + 4, len);
        }
    };
    while buf.len() < head_end + body_len {
        let n = s.read(&mut chunk)?;
        if n == 0 {
            break;
        }
        buf.extend_from_slice(&chunk[..n]);
    }
    let head = String::from_utf8_lossy(&buf[..head_end]).to_string();
    let mut line = head.lines().next().unwrap_or("").split(' ');
    let (method, target) = (line.next().unwrap_or(""), line.next().unwrap_or("/"));
    let mut params: BTreeMap<String, String> = BTreeMap::new();
    if let Some((_, q)) = target.split_once('?') {
        params.extend(form_urlencoded::parse(q.as_bytes()).into_owned());
    }
    if method == "POST" {
        let end = (head_end + body_len).min(buf.len());
        params.extend(form_urlencoded::parse(&buf[head_end..end]).into_owned());
    }
    let body = match params.get("action").map(String::as_str) {
        Some("getInfo") => get_info(st),
        Some("addUser") => add_user(st, &params),
        _ => return respond(&mut s, 404, ""),
    };
    respond(&mut s, 200, &body)
}

fn respond(s: &mut TcpStream, code: u16, body: &str) -> std::io::Result<()> {
    let reason = match code { 200 => "OK", 404 => "Not Found", _ => "Error" };
    write!(s, "HTTP/1.1 {code} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len())?;
    s.flush()
}

fn find(h: &[u8], n: &[u8]) -> Option<usize> {
    h.windows(n.len()).position(|w| w == n)
}

fn get_info(st: &State) -> String {
    // See: https://developer.spotify.com/documentation/commercial-hardware/implementation/guides/zeroconf/
    json!({
        "status": 101, "statusString": "OK", "spotifyError": 0,
        "version": "2.9.0",
        "deviceID": st.config.device_id,
        "deviceType": "SPEAKER",
        "remoteName": st.config.name,
        "publicKey": BASE64.encode(st.keys.public_key()),
        "brandDisplayName": "hassmic",
        "modelDisplayName": "Echo",
        "libraryVersion": librespot_core::version::SEMVER,
        "resolverVersion": "1",
        "groupStatus": "NONE",
        "tokenType": "default",
        "clientID": st.config.client_id,
        "productID": 0,
        "scope": "streaming",
        "availability": "",
        "supported_drm_media_formats": [],
        "supported_capabilities": 1,
        "accountReq": "PREMIUM",
        "activeUser": st.user.lock().map(|u| u.clone()).unwrap_or_default(),
        "aliases": [],
    })
    .to_string()
}

fn error(code: i32, s: &str) -> String {
    json!({ "status": code, "spotifyError": 1, "statusString": s }).to_string()
}

fn add_user(st: &State, p: &BTreeMap<String, String>) -> String {
    let (Some(user), Some(blob), Some(key)) = (p.get("userName"), p.get("blob"), p.get("clientKey")) else {
        return error(102, "ERROR-MISSING-PARAMS");
    };
    let (Ok(blob), Ok(key)) = (BASE64.decode(blob.as_bytes()), BASE64.decode(key.as_bytes())) else {
        return error(102, "ERROR-BASE64");
    };
    match decrypt(&st.keys.shared_secret(&key), &blob) {
        Some(plain) => match Credentials::with_blob(user, plain, &st.config.device_id) {
            Ok(c) => {
                if let Ok(mut u) = st.user.lock() {
                    *u = user.clone();
                }
                log::info!("zeroconf: login handed over for {user}");
                let _ = st.tx.send(c);
                json!({ "status": 101, "spotifyError": 0, "statusString": "OK" }).to_string()
            }
            Err(e) => {
                log::warn!("zeroconf: unusable credentials for {user}: {e}");
                error(102, "ERROR-BLOB")
            }
        },
        None => {
            log::warn!("zeroconf: login for {user}: MAC mismatch");
            error(102, "ERROR-MAC")
        }
    }
}

/// addUser's blob: IV (16) + AES-128-CTR data + HMAC-SHA1 (20), keys from SHA1 of the DH secret.  None: MAC mismatch
pub fn decrypt(shared: &[u8], blob: &[u8]) -> Option<Vec<u8>> {
    if blob.len() < 36 {
        return None;
    }
    let (iv, rest) = blob.split_at(16);
    let (data, mac) = rest.split_at(rest.len() - 20);
    let base = Sha1::digest(shared);
    let base = &base[..16];
    let derive = |label: &[u8]| {
        let mut h = <Hmac<Sha1> as Mac>::new_from_slice(base).ok()?;
        h.update(label);
        Some(h.finalize().into_bytes())
    };
    let (ck, ek) = (derive(b"checksum")?, derive(b"encryption")?);
    let mut h = <Hmac<Sha1> as Mac>::new_from_slice(&ck).ok()?;
    h.update(data);
    h.verify_slice(mac).ok()?;
    let mut out = data.to_vec();
    Aes128Ctr::new_from_slices(&ek[..16], iv).ok()?.apply_keystream(&mut out);
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The phone's side of addUser, as librespot's own tests and the Spotify apps build it
    fn seal(shared: &[u8], iv: [u8; 16], plain: &[u8]) -> Vec<u8> {
        let base = Sha1::digest(shared);
        let mac = |k: &[u8], d: &[u8]| {
            let mut h = <Hmac<Sha1> as Mac>::new_from_slice(k).unwrap();
            h.update(d);
            h.finalize().into_bytes().to_vec()
        };
        let ck = mac(&base[..16], b"checksum");
        let ek = mac(&base[..16], b"encryption");
        let mut data = plain.to_vec();
        Aes128Ctr::new_from_slices(&ek[..16], &iv).unwrap().apply_keystream(&mut data);
        let tag = mac(&ck, &data);
        [iv.to_vec(), data, tag].concat()
    }

    #[test]
    fn decrypts_what_a_phone_seals() {
        let ours = DhLocalKeys::random(&mut rand::rng());
        let phone = DhLocalKeys::random(&mut rand::rng());
        let blob = seal(&phone.shared_secret(&ours.public_key()), [7; 16], b"a blob of the account");
        assert_eq!(decrypt(&ours.shared_secret(&phone.public_key()), &blob).as_deref(), Some(&b"a blob of the account"[..]));
    }

    /// librespot-core's hash cash as patched here (src/third_party/librespot-core): on every core, still an answer
    /// whose hash has the trailing zeros, in the suffix's form (target + counter, counter)
    #[test]
    fn hash_cash_on_every_core() {
        use byteorder_free::read_i64;
        let (ctx, prefix) = (b"a context from Spotify", b"prefix of it");
        let mut suffix = [0u8; 16];
        librespot_core::util::solve_hash_cash(ctx, prefix, 14, &mut suffix).expect("solved within 5 s");
        let mut h = Sha1::new();
        h.update(prefix);
        h.update(suffix);
        assert!(read_i64(&h.finalize()[12..20]).trailing_zeros() >= 14);
        let target = read_i64(&Sha1::digest(ctx)[12..20]);
        assert_eq!(read_i64(&suffix[..8]), target.wrapping_add(read_i64(&suffix[8..])));
    }

    mod byteorder_free {
        pub fn read_i64(b: &[u8]) -> i64 { i64::from_be_bytes(b[..8].try_into().unwrap()) }
    }

    #[test]
    fn refuses_a_changed_blob() {
        let ours = DhLocalKeys::random(&mut rand::rng());
        let mut blob = seal(&ours.shared_secret(&ours.public_key()), [1; 16], b"secret");
        blob[20] ^= 1;
        assert!(decrypt(&ours.shared_secret(&ours.public_key()), &blob).is_none());
        assert!(decrypt(b"x", &[0; 10]).is_none());
    }
}
