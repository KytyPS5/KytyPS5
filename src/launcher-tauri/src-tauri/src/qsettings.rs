//! Minimal reimplementation of Qt's `QSettings::IniFormat` text encoding.
//!
//! Verified empirically against Qt 6 (see `/tmp` probe programs used while
//! building this module — not shipped) rather than assumed from memory:
//! Qt's ini writer does not do a surgical/partial rewrite. On every `sync()`
//! it dumps its *entire* in-memory settings tree, sections sorted
//! alphabetically, keys within a section sorted alphabetically. Array
//! entries use a literal backslash separator (`1\name=...`, 1-based) plus a
//! `size=N` key. `QStringList` values join items with `", "`; an item is
//! wrapped in double quotes when it contains `,`, `=`, `;`, or has leading
//! or trailing whitespace; `\`, `"`, and control characters are always
//! backslash-escaped regardless of quoting; an empty list serializes as
//! `@Invalid()`.
//!
//! Because Qt itself fully regenerates the file this way, matching that
//! canonical form is both simpler and safer than trying to preserve byte
//! ranges we don't understand: any key/value pair we don't decode (window
//! geometry blobs, future Qt-only keys, etc.) is carried through as an
//! opaque raw string and re-emitted unchanged in its sorted position.

use std::collections::BTreeMap;

/// `[Section] -> key -> raw value text` (everything after the first `=`,
/// undecoded). Sections and keys are stored in a `BTreeMap` so iteration
/// order is already the ASCII-sorted order Qt itself writes.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct IniDocument(pub BTreeMap<String, BTreeMap<String, String>>);

impl IniDocument {
    pub fn parse(text: &str) -> Self {
        let mut doc: BTreeMap<String, BTreeMap<String, String>> = BTreeMap::new();
        let mut section = String::new();

        for line in text.lines() {
            let trimmed = line.trim();
            if trimmed.is_empty() {
                continue;
            }
            if trimmed.starts_with('[') && trimmed.ends_with(']') {
                section = trimmed[1..trimmed.len() - 1].to_string();
                doc.entry(section.clone()).or_default();
                continue;
            }
            if let Some(eq) = line.find('=') {
                let key = line[..eq].to_string();
                let value = line[eq + 1..].to_string();
                doc.entry(section.clone()).or_default().insert(key, value);
            }
            // Anything else (stray comment, malformed line) is dropped: Qt's
            // own ini writer never produces such lines, so none of our real
            // input has any either.
        }

        IniDocument(doc)
    }

    pub fn serialize(&self) -> String {
        let mut out = String::new();
        let mut first = true;
        for (section, keys) in &self.0 {
            if !first {
                out.push('\n');
            }
            first = false;
            out.push('[');
            out.push_str(section);
            out.push_str("]\n");
            for (key, value) in keys {
                out.push_str(key);
                out.push('=');
                out.push_str(value);
                out.push('\n');
            }
        }
        out
    }

    pub fn get(&self, section: &str, key: &str) -> Option<&str> {
        self.0.get(section)?.get(key).map(String::as_str)
    }

    pub fn set(&mut self, section: &str, key: &str, raw_value: String) {
        self.0.entry(section.to_string()).or_default().insert(key.to_string(), raw_value);
    }

    /// Drop a single key, leaving the rest of its section alone. Needed for
    /// settings that are meant to be *absent* rather than written as a
    /// falsy value, so that a Kyty.ini the Qt launcher wrote round-trips
    /// through this one byte-for-byte.
    pub fn remove(&mut self, section: &str, key: &str) {
        if let Some(entries) = self.0.get_mut(section) {
            entries.remove(key);
        }
    }

    pub fn remove_section(&mut self, section: &str) {
        self.0.remove(section);
    }

    pub fn ensure_section(&mut self, section: &str) {
        self.0.entry(section.to_string()).or_default();
    }
}

fn escape_value(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            _ => out.push(c),
        }
    }
    out
}

fn unescape_value(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars.next() {
                Some('\\') => out.push('\\'),
                Some('"') => out.push('"'),
                Some('n') => out.push('\n'),
                Some('r') => out.push('\r'),
                Some('t') => out.push('\t'),
                Some(other) => {
                    out.push('\\');
                    out.push(other);
                }
                None => out.push('\\'),
            }
        } else {
            out.push(c);
        }
    }
    out
}

fn needs_quotes(s: &str) -> bool {
    s.contains(',')
        || s.contains('=')
        || s.contains(';')
        || s.starts_with(' ')
        || s.starts_with('\t')
        || s.ends_with(' ')
        || s.ends_with('\t')
}

fn encode_item(s: &str) -> String {
    let escaped = escape_value(s);
    if needs_quotes(s) {
        format!("\"{escaped}\"")
    } else {
        escaped
    }
}

fn decode_item(part: &str) -> String {
    if part.len() >= 2 && part.starts_with('"') && part.ends_with('"') {
        unescape_value(&part[1..part.len() - 1])
    } else {
        unescape_value(part)
    }
}

/// Split on commas that are not inside a (possibly escaped-quote-containing)
/// quoted region, trimming the single space Qt always writes after a comma.
fn split_top_level(s: &str) -> Vec<String> {
    if s.is_empty() {
        return vec![];
    }
    let mut parts = Vec::new();
    let mut cur = String::new();
    let mut in_quotes = false;
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\\' => {
                cur.push(c);
                if let Some(&next) = chars.peek() {
                    cur.push(next);
                    chars.next();
                }
            }
            '"' => {
                in_quotes = !in_quotes;
                cur.push(c);
            }
            ',' if !in_quotes => {
                parts.push(std::mem::take(&mut cur));
                if chars.peek() == Some(&' ') {
                    chars.next();
                }
            }
            _ => cur.push(c),
        }
    }
    parts.push(cur);
    parts
}

/// Encode a scalar `QString` value the way `QVariant::fromValue(x).toString()`
/// gets written by the ini backend. An empty string writes as a bare `key=`.
pub fn encode_string(s: &str) -> String {
    encode_item(s)
}

pub fn decode_string(raw: &str) -> String {
    decode_item(raw)
}

/// Encode a `QStringList`. Empty ⇒ Qt's `@Invalid()` null-variant marker.
pub fn encode_string_list(items: &[String]) -> String {
    if items.is_empty() {
        "@Invalid()".to_string()
    } else {
        items.iter().map(|s| encode_item(s)).collect::<Vec<_>>().join(", ")
    }
}

pub fn decode_string_list(raw: &str) -> Vec<String> {
    let trimmed = raw.trim();
    if trimmed.is_empty() || trimmed == "@Invalid()" {
        return vec![];
    }
    split_top_level(raw).iter().map(|p| decode_item(p)).collect()
}

pub fn encode_bool(b: bool) -> String {
    if b { "true" } else { "false" }.to_string()
}

pub fn decode_bool(raw: &str) -> bool {
    raw.trim() == "true"
}

pub fn encode_int(i: i64) -> String {
    i.to_string()
}

pub fn decode_int(raw: &str, default: i64) -> i64 {
    raw.trim().parse().unwrap_or(default)
}

pub fn encode_f64(f: f64) -> String {
    f.to_string()
}

pub fn decode_f64(raw: &str, default: f64) -> f64 {
    raw.trim().parse().unwrap_or(default)
}

/// Array-index key prefix Qt's `beginWriteArray`/`setArrayIndex` produces:
/// a literal backslash, e.g. `array_key("GameConfigurations", 1, "name")`
/// addresses the raw key `"1\name"` inside section `GameConfigurations`.
pub fn array_key(index: usize, field: &str) -> String {
    format!("{index}\\{field}")
}

#[cfg(test)]
mod tests {
    use super::*;

    const REAL_KYTY_INI: &str = include_str!("../fixtures/real_kyty_empty.ini");
    const PROBE_ARRAY_INI: &str = include_str!("../fixtures/probe_array.ini");
    const PROBE_CHARS_INI: &str = include_str!("../fixtures/probe_chars.ini");

    #[test]
    fn round_trip_real_file_is_byte_identical() {
        let doc = IniDocument::parse(REAL_KYTY_INI);
        let out = doc.serialize();
        assert_eq!(out.trim_end(), REAL_KYTY_INI.trim_end());
    }

    #[test]
    fn round_trip_array_probe_is_byte_identical() {
        let doc = IniDocument::parse(PROBE_ARRAY_INI);
        let out = doc.serialize();
        assert_eq!(out.trim_end(), PROBE_ARRAY_INI.trim_end());
    }

    #[test]
    fn round_trip_special_chars_probe_is_byte_identical() {
        let doc = IniDocument::parse(PROBE_CHARS_INI);
        let out = doc.serialize();
        assert_eq!(out.trim_end(), PROBE_CHARS_INI.trim_end());
    }

    #[test]
    fn string_list_special_chars_match_qt() {
        let items = vec![
            "plain".to_string(),
            "has=equals".to_string(),
            "has,comma".to_string(),
            "has;semicolon".to_string(),
            "has#hash".to_string(),
            "has\"quote".to_string(),
            "has\\backslash".to_string(),
            "has space".to_string(),
            " leadspace".to_string(),
            "trailspace ".to_string(),
            "has\nnewline".to_string(),
            String::new(),
            "has:colon".to_string(),
            "[brackets]".to_string(),
        ];
        let encoded = encode_string_list(&items);
        assert_eq!(
            encoded,
            "plain, \"has=equals\", \"has,comma\", \"has;semicolon\", has#hash, \
             has\\\"quote, has\\\\backslash, has space, \" leadspace\", \"trailspace \", \
             has\\nnewline, , has:colon, [brackets]"
        );
        assert_eq!(decode_string_list(&encoded), items);
    }

    #[test]
    fn empty_list_is_invalid_marker() {
        assert_eq!(encode_string_list(&[]), "@Invalid()");
        assert_eq!(decode_string_list("@Invalid()"), Vec::<String>::new());
    }

    #[test]
    fn array_key_uses_backslash_separator() {
        assert_eq!(array_key(1, "name"), "1\\name");
        assert_eq!(array_key(2, "basedir"), "2\\basedir");
    }

    #[test]
    fn game_configurations_array_decodes() {
        let doc = IniDocument::parse(PROBE_ARRAY_INI);
        assert_eq!(doc.get("GameConfigurations", "size"), Some("2"));
        assert_eq!(doc.get("GameConfigurations", "1\\basedir"), Some("/mnt/games/g0"));
        assert_eq!(doc.get("GameConfigurations", "2\\vblank_frequency"), Some("61"));
        let mapping = decode_string_list(
            doc.get("GameConfigurations", "1\\host_input_mapping").unwrap(),
        );
        assert_eq!(mapping, vec!["Cross=J".to_string(), "Circle=L,extra".to_string()]);
    }

    #[test]
    fn unknown_keys_pass_through_opaque() {
        let doc = IniDocument::parse(REAL_KYTY_INI);
        // The window-geometry ByteArray blob is never decoded, only carried
        // through byte-for-byte.
        let geometry = doc.get("MainDialog", "geometry").unwrap();
        assert!(geometry.starts_with("@ByteArray("));
    }
}
