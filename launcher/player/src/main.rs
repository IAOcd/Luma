// =============================================================================
//  luma-spotify - Luma's own Spotify player
// =============================================================================
//  A small program (no window) that becomes a Spotify Connect device called
//  "Luma" and plays the music through Windows' sound. It's built on
//  librespot (github.com/librespot-org/librespot, MIT), the same engine
//  Spotifast's built-in player uses. librespot is NOT made by Spotify: it
//  speaks Spotify's own protocol.
//
//  The first time it opens Spotify's sign-in page in your browser (you
//  approve "Luma" there) and keeps a reusable pass in its cache folder, so the
//  next times it connects by itself.
//
//  The launcher runs it hidden and reads what it says, one line at a time:
//      status: login      (the browser was opened: approve there)
//      status: connecting
//      status: ready      (the "Luma" device exists)
//      error: ...
//
//  The launcher can also ask it directly, one line at a time on its input:
//      load <context-uri> <track-uri or -> <position-ms>   (play an album, playlist, your Liked songs...)
//      tracks <index> <position-ms> <track-uri> <track-uri> ...
//      volume <0-100> | play | pause | next | prev
//      ask <request> playlist|radio|artist_top <arg>  -> "answer <request> {json}" (see answer())
//
//  Usage: luma-spotify --name Luma --cache <folder> [--volume 0-100] [--quality 160|320]
// =============================================================================

use std::io::Write;

use librespot::core::SpotifyUri;
use librespot::metadata::{Artist, Metadata, Playlist, Track};

use librespot::{
    connect::{ConnectConfig, LoadRequest, LoadRequestOptions, PlayingTrack, Spirc},
    core::{authentication::Credentials, cache::Cache, config::DeviceType, config::SessionConfig, session::Session, Error},
    playback::{
        audio_backend,
        config::{AudioFormat, Bitrate, PlayerConfig},
        mixer::{self, MixerConfig},
        player::Player,
    },
};

// librespot's own warnings and errors, as lines for the launcher's log.
struct LineLogger;
impl log::Log for LineLogger {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Warn || (m.target().starts_with("librespot") && m.level() <= log::Level::Info)
    }
    fn log(&self, r: &log::Record) {
        if self.enabled(r.metadata()) {
            say(&format!("log: [{}] {}: {}", r.level(), r.target(), r.args()));
        }
    }
    fn flush(&self) {}
}
static LOGGER: LineLogger = LineLogger;

fn say(line: &str) {
    let mut out = std::io::stdout();
    let _ = writeln!(out, "{line}");
    let _ = out.flush();
}

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

// Songs (name, artists, album, length, cover) of these URIs, read through
// Spotify's internal channel, 16 at a time, in their order.
async fn tracks_json(session: &Session, uris: Vec<SpotifyUri>) -> Vec<serde_json::Value> {
    let mut songs: Vec<(usize, serde_json::Value)> = Vec::new();
    for (chunk_index, chunk) in uris.chunks(16).enumerate() {
        let mut tasks = tokio::task::JoinSet::new();
        for (i, u) in chunk.iter().enumerate() {
            let session = session.clone();
            let u = u.clone();
            let index = chunk_index * 16 + i;
            tasks.spawn(async move {
                let track = Track::get(&session, &u).await.ok()?;
                let artists: Vec<serde_json::Value> = track
                    .artists
                    .iter()
                    .map(|a| serde_json::json!({"id": a.id.to_id().unwrap_or_default(), "name": a.name}))
                    .collect();
                let cover = track
                    .album
                    .covers
                    .iter()
                    .min_by_key(|c| (c.width - 300).abs())
                    .and_then(|c| c.id.to_base16().ok())
                    .map(|hex| format!("https://i.scdn.co/image/{hex}"))
                    .unwrap_or_default();
                Some((
                    index,
                    serde_json::json!({
                        "type": "track",
                        "id": track.id.to_id().unwrap_or_default(),
                        "uri": track.id.to_uri().unwrap_or_default(),
                        "name": track.name,
                        "duration_ms": track.duration,
                        "artists": artists,
                        "album": {"id": track.album.id.to_id().unwrap_or_default(), "name": track.album.name,
                                  "images": [{"url": cover, "width": 300}]}
                    }),
                ))
            });
        }
        while let Some(done) = tasks.join_next().await {
            if let Ok(Some(song)) = done {
                songs.push(song);
            }
        }
    }
    songs.sort_by_key(|(i, _)| *i);
    songs.into_iter().map(|(_, t)| t).collect()
}

fn as_list(name: &str, tracks: Vec<serde_json::Value>) -> serde_json::Value {
    let items: Vec<serde_json::Value> = tracks.into_iter().map(|t| serde_json::json!({"item": t})).collect();
    serde_json::json!({"name": name, "items": {"items": items, "total": items.len()}})
}

// THE LAUNCHER'S QUESTIONS (through Spotify's internal channel, like
// Spotify's own apps: the public Web API closed these to apps in 2026):
//   playlist <id>      its songs
//   radio <uri>        Spotify's own radio of a song / artist / album / playlist: {"uri": playlist}
//   artist_top <id>    the artist's popular songs
// Answered as "answer <request> {json}" ({"error": ...} if it failed).
async fn answer(session: Session, request: String, what: String, arg: String) {
    let result: Result<serde_json::Value, String> = async {
        match what.as_str() {
            "playlist" => {
                let uri = SpotifyUri::from_uri(&format!("spotify:playlist:{arg}")).map_err(|e| e.to_string())?;
                let list = Playlist::get(&session, &uri).await.map_err(|e| e.to_string())?;
                let uris: Vec<SpotifyUri> = list.tracks().take(500).cloned().collect();
                Ok(as_list(&list.attributes.name, tracks_json(&session, uris).await))
            }
            "radio" => {
                let seed = SpotifyUri::from_uri(&arg).map_err(|e| e.to_string())?;
                let bytes = session.spclient().get_radio_for_track(&seed).await.map_err(|e| e.to_string())?;
                let j: serde_json::Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
                let uri = j["mediaItems"][0]["uri"].as_str().ok_or("no radio")?.to_string();
                Ok(serde_json::json!({"uri": uri}))
            }
            "artist_top" => {
                let uri = SpotifyUri::from_uri(&format!("spotify:artist:{arg}")).map_err(|e| e.to_string())?;
                let artist = Artist::get(&session, &uri).await.map_err(|e| e.to_string())?;
                let uris: Vec<SpotifyUri> = artist.top_tracks.for_country(&session.country()).iter().take(10).cloned().collect();
                Ok(as_list(&artist.name, tracks_json(&session, uris).await))
            }
            _ => Err("unknown question".to_string()),
        }
    }
    .await;
    match result {
        Ok(j) => say(&format!("answer {request} {j}")),
        Err(e) => say(&format!("answer {request} {}", serde_json::json!({"error": e}))),
    }
}

// The page your browser shows after you approve.
const DONE_PAGE: &str = "<!doctype html><html><head><meta charset=\"utf-8\"><title>Luma</title><style>\
body{background:#1B1D1F;color:#E9E6DF;font-family:Segoe UI,sans-serif;display:flex;align-items:center;\
justify-content:center;height:100vh;margin:0}div{text-align:center}h1{font-weight:600}p{opacity:.6}</style></head>\
<body><div><h1>Luma + Spotify</h1><p>You can close this tab and go back to Luma.</p>\
<p>Ya puedes cerrar esta pesta&ntilde;a y volver a Luma.</p></div></body></html>";

async fn run() -> Result<(), Error> {
    let args: Vec<String> = std::env::args().collect();
    let name = arg(&args, "--name").unwrap_or_else(|| "Luma".to_string());
    let cache_dir = arg(&args, "--cache").unwrap_or_else(|| ".cache".to_string());
    let volume: u16 = arg(&args, "--volume").and_then(|v| v.parse::<u32>().ok()).map(|v| (v.min(100) * 65535 / 100) as u16).unwrap_or(u16::MAX);
    let high = arg(&args, "--quality").map(|q| q == "320").unwrap_or(true);

    let session_config = SessionConfig::default();
    let audio_dir = format!("{cache_dir}/audio");
    let cache = Cache::new(Some(cache_dir.as_str()), Some(cache_dir.as_str()), Some(audio_dir.as_str()), Some(500 * 1024 * 1024))?;

    // Your pass: kept from before, or a new one (Spotify's page in your browser).
    let credentials = match cache.credentials() {
        Some(c) => c,
        None => {
            say("status: login");
            let token = librespot_oauth::OAuthClientBuilder::new(&session_config.client_id, "http://127.0.0.1:8898/login", vec!["streaming"])
                .open_in_browser()
                .with_custom_message(DONE_PAGE)
                .build()
                .map_err(|e| Error::unavailable(format!("{e}")))?
                .get_access_token()
                .map_err(|e| Error::unavailable(format!("{e}")))?;
            Credentials::with_access_token(token.access_token)
        }
    };
    say("status: connecting");

    let session = Session::new(session_config, Some(cache));
    let mixer = mixer::find(None).ok_or_else(|| Error::unavailable("no mixer"))?(MixerConfig::default())?;
    let sink_builder = audio_backend::find(None).ok_or_else(|| Error::unavailable("no audio output"))?;
    let player_config = PlayerConfig { bitrate: if high { Bitrate::Bitrate320 } else { Bitrate::Bitrate160 }, ..PlayerConfig::default() };
    let audio_format = AudioFormat::default();
    let player = Player::new(player_config, session.clone(), mixer.get_soft_volume(), move || sink_builder(None, audio_format));
    // what the player does (for the launcher's log)
    let mut events = player.get_player_event_channel();
    tokio::spawn(async move {
        while let Some(e) = events.recv().await {
            let text = format!("{e:?}");
            if !text.starts_with("PositionChanged") {
                say(&format!("event: {}", text.chars().take(160).collect::<String>()));
            }
        }
    });

    let connect_config = ConnectConfig { name, device_type: DeviceType::Computer, initial_volume: volume, ..ConnectConfig::default() };
    let (spirc, spirc_task) = Spirc::new(connect_config, session.clone(), credentials, player, mixer).await?;
    say("status: ready");
    // what the launcher asks (its lines, on our input)
    let runtime = tokio::runtime::Handle::current();
    let reader_session = session.clone();
    std::thread::spawn(move || {
        for line in std::io::stdin().lines() {
            let Ok(line) = line else { break };
            let parts: Vec<&str> = line.split_whitespace().collect();
            let result = match parts.as_slice() {
                ["load", context, track, position, ..] => {
                    let options = LoadRequestOptions {
                        start_playing: true,
                        seek_to: position.parse().unwrap_or(0),
                        context_options: None,
                        playing_track: if *track == "-" { None } else { Some(PlayingTrack::Uri(track.to_string())) },
                    };
                    let _ = spirc.activate();
                    spirc.load(LoadRequest::from_context_uri(context.to_string(), options))
                }
                ["tracks", index, position, uris @ ..] if !uris.is_empty() => {
                    let options = LoadRequestOptions {
                        start_playing: true,
                        seek_to: position.parse().unwrap_or(0),
                        context_options: None,
                        playing_track: Some(PlayingTrack::Index(index.parse().unwrap_or(0))),
                    };
                    let _ = spirc.activate();
                    spirc.load(LoadRequest::from_tracks(uris.iter().map(|u| u.to_string()).collect(), options))
                }
                ["ask", request, what, arg] => {
                    runtime.spawn(answer(reader_session.clone(), request.to_string(), what.to_string(), arg.to_string()));
                    Ok(())
                }
                ["volume", v] => spirc.set_volume((v.parse::<u32>().unwrap_or(100).min(100) * 65535 / 100) as u16),
                ["play"] => spirc.play(),
                ["pause"] => spirc.pause(),
                ["next"] => spirc.next(),
                ["prev"] => spirc.prev(),
                _ => Ok(()),
            };
            if let Err(e) = result {
                say(&format!("log: [ERROR] command \"{}\": {e}", parts.first().unwrap_or(&"")));
            }
        }
    });
    spirc_task.await;
    say("error: the connection ended");
    Ok(())
}

#[tokio::main]
async fn main() {
    let _ = log::set_logger(&LOGGER).map(|()| log::set_max_level(log::LevelFilter::Info));
    if let Err(e) = run().await {
        say(&format!("error: {e}"));
        std::process::exit(1);
    }
}
