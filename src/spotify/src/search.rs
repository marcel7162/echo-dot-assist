//! "Spiel <etwas> auf Spotify": what to play for spoken words, by Spotify's Web API search (GET /v1/search) with the
//! session's own token.  Speech to text gives a name, rarely spelled as Spotify spells it, and no kind unless one was
//! said ("die Playlist ...", "das Album ..."): an exact name in the order artist, playlist, album, track wins, else
//! Spotify's best track (its ranking already reads "Thriller Michael Jackson" as a song and its artist).

use serde_json::Value;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Any,
    Track,
    Artist,
    Album,
    Playlist,
}

impl Kind {
    pub fn parse(s: &str) -> Option<Kind> {
        Some(match s {
            "any" => Kind::Any,
            "track" => Kind::Track,
            "artist" => Kind::Artist,
            "album" => Kind::Album,
            "playlist" => Kind::Playlist,
            _ => return None,
        })
    }
    fn api(self) -> &'static str {
        match self {
            Kind::Any => "artist,playlist,album,track",
            Kind::Track => "track",
            Kind::Artist => "artist",
            Kind::Album => "album",
            Kind::Playlist => "playlist",
        }
    }
    fn key(self) -> &'static str {
        match self {
            Kind::Track => "tracks",
            Kind::Artist => "artists",
            Kind::Album => "albums",
            Kind::Playlist => "playlists",
            Kind::Any => "",
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Kind::Any => "any",
            Kind::Track => "track",
            Kind::Artist => "artist",
            Kind::Album => "album",
            Kind::Playlist => "playlist",
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct Choice {
    pub kind: Kind,
    pub uri: String,
    pub name: String,
}

pub fn url(kind: Kind, query: &str) -> String {
    let q: String = form_urlencoded::byte_serialize(query.as_bytes()).collect();
    format!("https://api.spotify.com/v1/search?q={q}&type={}&limit=5&market=from_token", kind.api())
}

/// Lower case letters and digits, one space between words, and what follows " - " or an opening bracket dropped:
/// "Bohemian Rhapsody - Remastered 2011" and "bohemian rhapsody" are the same song to a listener
pub fn norm(s: &str) -> String {
    let s = s.split(" - ").next().unwrap_or(s);
    let s = s.split(['(', '[']).next().unwrap_or(s);
    let mut out = String::new();
    for w in s.split(|c: char| !c.is_alphanumeric()).filter(|w| !w.is_empty()) {
        if !out.is_empty() {
            out.push(' ');
        }
        out.extend(w.chars().flat_map(char::to_lowercase));
    }
    out
}

fn items<'a>(v: &'a Value, kind: Kind) -> impl Iterator<Item = &'a Value> {
    // playlists' items may be null (removed or hidden ones): skipped
    v.get(kind.key()).and_then(|k| k.get("items")).and_then(Value::as_array).into_iter().flatten().filter(|i| i.get("uri").and_then(Value::as_str).is_some())
}

fn choice(kind: Kind, i: &Value) -> Choice {
    let s = |k: &str| i.get(k).and_then(Value::as_str).unwrap_or("").to_string();
    let mut name = s("name");
    if kind == Kind::Track || kind == Kind::Album {
        if let Some(a) = i.get("artists").and_then(Value::as_array).and_then(|a| a.first()).and_then(|a| a.get("name")).and_then(Value::as_str) {
            name = format!("{name} – {a}");
        }
    }
    Choice { kind, uri: s("uri"), name }
}

pub fn pick(v: &Value, kind: Kind, query: &str) -> Option<Choice> {
    if kind != Kind::Any {
        return items(v, kind).next().map(|i| choice(kind, i));
    }
    let q = norm(query);
    for k in [Kind::Artist, Kind::Playlist, Kind::Album, Kind::Track] {
        if let Some(i) = items(v, k).find(|i| norm(i.get("name").and_then(Value::as_str).unwrap_or("")) == q) {
            return Some(choice(k, i));
        }
    }
    for k in [Kind::Track, Kind::Artist, Kind::Playlist, Kind::Album] {
        if let Some(i) = items(v, k).next() {
            return Some(choice(k, i));
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn answer() -> Value {
        json!({
            "tracks": { "items": [
                { "name": "Thriller", "uri": "spotify:track:t1", "artists": [{ "name": "Michael Jackson" }] },
                { "name": "Bohemian Rhapsody - Remastered 2011", "uri": "spotify:track:t2", "artists": [{ "name": "Queen" }] }
            ]},
            "artists": { "items": [
                { "name": "Queen", "uri": "spotify:artist:a1" },
                { "name": "Queens of the Stone Age", "uri": "spotify:artist:a2" }
            ]},
            "albums": { "items": [{ "name": "Thriller (Special Edition)", "uri": "spotify:album:b1", "artists": [{ "name": "Michael Jackson" }] }]},
            "playlists": { "items": [null, { "name": "Chillout Lounge", "uri": "spotify:playlist:p1" }]}
        })
    }

    #[test]
    fn exact_names_by_kind_order() {
        let v = answer();
        assert_eq!(pick(&v, Kind::Any, "Queen").unwrap().uri, "spotify:artist:a1");
        assert_eq!(pick(&v, Kind::Any, "chillout lounge").unwrap().uri, "spotify:playlist:p1");
        // the album's "(Special Edition)" goes, so the album named as said comes before the song
        assert_eq!(pick(&v, Kind::Any, "Thriller").unwrap().uri, "spotify:album:b1");
        assert_eq!(pick(&v, Kind::Any, "bohemian rhapsody").unwrap().uri, "spotify:track:t2");
    }

    #[test]
    fn otherwise_spotifys_best_track() {
        let c = pick(&answer(), Kind::Any, "Thriller Michael Jackson").unwrap();
        assert_eq!((c.kind, c.uri.as_str(), c.name.as_str()), (Kind::Track, "spotify:track:t1", "Thriller – Michael Jackson"));
    }

    #[test]
    fn a_kind_said_is_taken() {
        let v = answer();
        assert_eq!(pick(&v, Kind::Playlist, "Queen").unwrap().uri, "spotify:playlist:p1");
        assert_eq!(pick(&v, Kind::Track, "Queen").unwrap().uri, "spotify:track:t1");
        assert!(pick(&json!({ "playlists": { "items": [null] } }), Kind::Playlist, "x").is_none());
        assert!(pick(&json!({}), Kind::Any, "x").is_none());
    }

    #[test]
    fn norm_and_url() {
        assert_eq!(norm("  Don't Stop Me Now [Live]"), "don t stop me now");
        assert_eq!(norm("Über Sieben Brücken"), "über sieben brücken");
        assert_eq!(url(Kind::Album, "a&b ü"), "https://api.spotify.com/v1/search?q=a%26b+%C3%BC&type=album&limit=5&market=from_token");
    }
}
