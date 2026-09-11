//! On-disk session logs, so a crash can be reported with a file attached
//! rather than a description from memory.
//!
//! Every launch gets its own `logs/session-YYYYMMDD-HHMMSS.log` under the
//! app data dir, written by whichever path actually started the emulator:
//! `emulator::spawn_in_app` tees the lines it is already streaming to the
//! in-app console, and `supervisor::run` redirects the child's stdout and
//! stderr straight into the file (there is no console left to stream to
//! once auto-close has taken the GUI away). Old files are pruned so this
//! never grows without bound.

use std::fs::File;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

/// How many session files to keep. Enough to cover "it broke a few runs
/// ago, which one was it", without turning into an unbounded pile.
const KEEP_SESSIONS: usize = 20;

const DIR_NAME: &str = "logs";

pub fn logs_dir(app_data_dir: &Path) -> PathBuf {
    app_data_dir.join(DIR_NAME)
}

/// `YYYYMMDD-HHMMSS` in UTC, from the civil-from-days algorithm rather than
/// a date crate: this is the only place the app formats a date, and one
/// filename is not worth a dependency (or the build-time cost of one).
fn timestamp(unix_seconds: u64) -> String {
    let days = (unix_seconds / 86_400) as i64;
    let seconds_of_day = unix_seconds % 86_400;

    // Howard Hinnant's civil_from_days, shifted to an era starting 0000-03-01.
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };

    format!(
        "{:04}{:02}{:02}-{:02}{:02}{:02}",
        y,
        m,
        d,
        seconds_of_day / 3600,
        (seconds_of_day % 3600) / 60,
        seconds_of_day % 60
    )
}

fn now_unix_seconds() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// Delete all but the newest `KEEP_SESSIONS` files. Sorted by name, which
/// is why the timestamp is big-endian: lexical order is chronological.
fn prune(dir: &Path) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    let mut sessions: Vec<PathBuf> = entries
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.starts_with("session-") && n.ends_with(".log"))
        })
        .collect();
    if sessions.len() <= KEEP_SESSIONS {
        return;
    }
    sessions.sort();
    for stale in &sessions[..sessions.len() - KEEP_SESSIONS] {
        let _ = std::fs::remove_file(stale);
    }
}

/// Path for a new session's log file, with the directory created and older
/// sessions pruned. Returns `None` only if the directory cannot be made, in
/// which case the caller carries on without a log rather than failing the
/// launch: not being able to write a log is never a reason not to play.
pub fn new_session_path(app_data_dir: &Path) -> Option<PathBuf> {
    let dir = logs_dir(app_data_dir);
    std::fs::create_dir_all(&dir).ok()?;
    prune(&dir);
    Some(dir.join(format!("session-{}.log", timestamp(now_unix_seconds()))))
}

/// A session log opened for writing, shared between the stdout and stderr
/// reader threads. Every write is best-effort: a full disk should not take
/// the running game down with it.
#[derive(Clone)]
pub struct SessionLog(Arc<Mutex<File>>);

impl SessionLog {
    pub fn create(path: &Path) -> Option<Self> {
        File::create(path).ok().map(|f| SessionLog(Arc::new(Mutex::new(f))))
    }

    pub fn write_line(&self, stream: &str, line: &str) {
        if let Ok(mut file) = self.0.lock() {
            // stderr is tagged rather than separated: interleaving order is
            // the useful part when diagnosing where a run went wrong.
            let _ = if stream == "stderr" {
                writeln!(file, "[stderr] {line}")
            } else {
                writeln!(file, "{line}")
            };
        }
    }

    pub fn write_header(&self, game_path: &str, args: &[String]) {
        if let Ok(mut file) = self.0.lock() {
            let _ = writeln!(file, "=== Kyty Launcher session {} ===", timestamp(now_unix_seconds()));
            let _ = writeln!(file, "game: {game_path}");
            let _ = writeln!(file, "args: {}", args.join(" "));
            let _ = writeln!(file, "---");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timestamp_is_sortable_and_correct() {
        // All UTC, deliberately: a log filename that shifts with the
        // reader's timezone is not a stable thing to sort or cite.
        assert_eq!(timestamp(1_789_167_967), "20260911-230607");
        // Epoch itself, and a date before the 2000 leap-year special case.
        assert_eq!(timestamp(0), "19700101-000000");
        assert_eq!(timestamp(951_782_400), "20000229-000000");
    }

    #[test]
    fn lexical_order_is_chronological() {
        let earlier = timestamp(1_789_167_967);
        let later = timestamp(1_789_167_968 + 86_400);
        assert!(earlier < later);
    }

    #[test]
    fn prune_keeps_only_the_newest_sessions() {
        let dir = tempfile::tempdir().unwrap();
        for i in 0..KEEP_SESSIONS + 5 {
            std::fs::write(dir.path().join(format!("session-2026090{i:02}-000000.log")), b"x").unwrap();
        }
        // An unrelated file must survive: pruning only owns what it named.
        std::fs::write(dir.path().join("notes.txt"), b"x").unwrap();

        prune(dir.path());

        let remaining: Vec<_> = std::fs::read_dir(dir.path())
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().to_string())
            .collect();
        assert_eq!(remaining.iter().filter(|n| n.starts_with("session-")).count(), KEEP_SESSIONS);
        assert!(remaining.iter().any(|n| n == "notes.txt"));
    }

    #[test]
    fn session_log_writes_lines_and_tags_stderr() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("s.log");
        let log = SessionLog::create(&path).unwrap();
        log.write_line("stdout", "hello");
        log.write_line("stderr", "boom");
        drop(log);

        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.contains("hello"));
        assert!(text.contains("[stderr] boom"));
    }
}
