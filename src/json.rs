//! 极简 JSON：只实现本工程需要的子集。
//! - `Json` 枚举承载解析结果；
//! - `Writer` 负责序列化（含字符串转义、紧凑输出）。
//! 不做科学计数法解析之外的花哨特性，但覆盖 API 的全部请求/响应。

use std::collections::BTreeMap;
use std::fmt::Write as _;

#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(BTreeMap<String, Json>),
    /// 非 JSON 负载：由 HTTP 层直接按对应 Content-Type 返回，不参与序列化。
    RawHtml(String),
    RawJs(String),
}

impl Json {
    /// 便捷构造对象，保持键顺序由 BTreeMap 决定（稳定可测）。
    pub fn obj(pairs: Vec<(&str, Json)>) -> Json {
        let mut m = BTreeMap::new();
        for (k, v) in pairs {
            m.insert(k.to_string(), v);
        }
        Json::Obj(m)
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Json::Num(n) => Some(*n as i64),
            _ => None,
        }
    }
    pub fn as_array(&self) -> Option<&Vec<Json>> {
        match self {
            Json::Arr(a) => Some(a),
            _ => None,
        }
    }
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(m) => m.get(key),
            _ => None,
        }
    }
    pub fn str_field(&self, key: &str) -> Option<&str> {
        self.get(key).and_then(|v| v.as_str())
    }
    pub fn i64_field(&self, key: &str) -> Option<i64> {
        self.get(key).and_then(|v| v.as_i64())
    }

    pub fn dump(&self) -> String {
        let mut out = String::new();
        self.write(&mut out);
        out
    }

    pub fn write(&self, out: &mut String) {
        match self {
            Json::Null => out.push_str("null"),
            Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
            Json::Num(n) => {
                if n.fract() == 0.0 && n.is_finite() && n.abs() < 1e15 {
                    let _ = write!(out, "{}", *n as i64);
                } else {
                    let _ = write!(out, "{n}");
                }
            }
            Json::Str(s) => write_json_string(s, out),
            Json::Arr(a) => {
                out.push('[');
                for (i, v) in a.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    v.write(out);
                }
                out.push(']');
            }
            Json::Obj(m) => {
                out.push('{');
                for (i, (k, v)) in m.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    write_json_string(k, out);
                    out.push(':');
                    v.write(out);
                }
                out.push('}');
            }
            Json::RawHtml(s) | Json::RawJs(s) => {
                // 静态资源不是 JSON；正常路径由 HTTP 层直接返回，
                // 走到这里说明被误当成 JSON，输出带标记的字符串以暴露问题。
                write_json_string(&format!("[non-json payload: {} bytes]", s.len()), out);
            }
        }
    }
}

fn write_json_string(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

// ---------- 解析器 ----------

struct Parser<'a> {
    b: &'a [u8],
    pos: usize,
}

pub fn parse(input: &str) -> Result<Json, String> {
    let mut p = Parser {
        b: input.as_bytes(),
        pos: 0,
    };
    p.skip_ws();
    let v = p.value()?;
    p.skip_ws();
    if p.pos != p.b.len() {
        return Err(format!("JSON 解析：位置 {} 后有多余字符", p.pos));
    }
    Ok(v)
}

impl<'a> Parser<'a> {
    fn skip_ws(&mut self) {
        while self.pos < self.b.len() && matches!(self.b[self.pos], b' ' | b'\t' | b'\n' | b'\r') {
            self.pos += 1;
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        self.skip_ws();
        if self.pos >= self.b.len() {
            return Err("JSON 解析：意外结束".into());
        }
        match self.b[self.pos] {
            b'{' => self.object(),
            b'[' => self.array(),
            b'"' => Ok(Json::Str(self.string()?)),
            b't' | b'f' => self.boolean(),
            b'n' => self.null(),
            b'-' | b'0'..=b'9' => self.number(),
            c => Err(format!("JSON 解析：位置 {} 出现意外字符 {c}", self.pos)),
        }
    }

    fn object(&mut self) -> Result<Json, String> {
        self.pos += 1; // {
        let mut map = BTreeMap::new();
        self.skip_ws();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(Json::Obj(map));
        }
        loop {
            self.skip_ws();
            if self.peek() != Some(b'"') {
                return Err("JSON 解析：对象键必须是字符串".into());
            }
            let key = self.string()?;
            self.skip_ws();
            if self.next() != Some(b':') {
                return Err("JSON 解析：对象中缺少 ':'".into());
            }
            let val = self.value()?;
            map.insert(key, val);
            self.skip_ws();
            match self.next() {
                Some(b',') => continue,
                Some(b'}') => break,
                other => return Err(format!("JSON 解析：对象分隔符错误，得到 {other:?}")),
            }
        }
        Ok(Json::Obj(map))
    }

    fn array(&mut self) -> Result<Json, String> {
        self.pos += 1; // [
        let mut items = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(Json::Arr(items));
        }
        loop {
            items.push(self.value()?);
            self.skip_ws();
            match self.next() {
                Some(b',') => continue,
                Some(b']') => break,
                other => return Err(format!("JSON 解析：数组分隔符错误，得到 {other:?}")),
            }
        }
        Ok(Json::Arr(items))
    }

    fn string(&mut self) -> Result<String, String> {
        self.pos += 1; // 开引号
        let mut s = String::new();
        loop {
            match self.next() {
                None => return Err("JSON 解析：字符串未闭合".into()),
                Some(b'"') => break,
                Some(b'\\') => match self.next() {
                    Some(b'"') => s.push('"'),
                    Some(b'\\') => s.push('\\'),
                    Some(b'/') => s.push('/'),
                    Some(b'n') => s.push('\n'),
                    Some(b't') => s.push('\t'),
                    Some(b'r') => s.push('\r'),
                    Some(b'b') => s.push('\u{08}'),
                    Some(b'f') => s.push('\u{0c}'),
                    Some(b'u') => {
                        let cp = self.hex4()?;
                        if (0xD800..=0xDBFF).contains(&cp) {
                            // 代理对
                            if self.next() != Some(b'\\') || self.next() != Some(b'u') {
                                return Err("JSON 解析：残缺的 UTF-16 代理对".into());
                            }
                            let lo = self.hex4()?;
                            let c = 0x10000
                                + (((cp - 0xD800) as u32) << 10)
                                + (lo - 0xDC00) as u32;
                            s.push(char::from_u32(c).ok_or("JSON 解析：非法码点")?);
                        } else {
                            s.push(char::from_u32(cp).ok_or("JSON 解析：非法码点")?);
                        }
                    }
                    _ => return Err("JSON 解析：非法转义".into()),
                },
                Some(c) => {
                    // 收集 UTF-8 字节
                    if c < 0x80 {
                        s.push(c as char);
                    } else {
                        let start = self.pos - 1;
                        let width = if c >= 0xF0 {
                            4
                        } else if c >= 0xE0 {
                            3
                        } else {
                            2
                        };
                        while self.pos < start + width && self.pos < self.b.len() {
                            self.pos += 1;
                        }
                        s.push_str(
                            std::str::from_utf8(&self.b[start..self.pos])
                                .map_err(|_| "JSON 解析：非法 UTF-8".to_string())?,
                        );
                    }
                }
            }
        }
        Ok(s)
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let mut v = 0u32;
        for _ in 0..4 {
            let c = self.next().ok_or("JSON 解析：\\u 后数字不足")?;
            v = v * 16
                + match c {
                    b'0'..=b'9' => (c - b'0') as u32,
                    b'a'..=b'f' => (c - b'a' + 10) as u32,
                    b'A'..=b'F' => (c - b'A' + 10) as u32,
                    _ => return Err("JSON 解析：\\u 后不是十六进制".into()),
                };
        }
        Ok(v)
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.pos;
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        while let Some(c) = self.peek() {
            if c.is_ascii_digit() || c == b'.' || c == b'e' || c == b'E' || c == b'+' || c == b'-' {
                self.pos += 1;
            } else {
                break;
            }
        }
        let text = std::str::from_utf8(&self.b[start..self.pos]).map_err(|e| e.to_string())?;
        text.parse::<f64>()
            .map(Json::Num)
            .map_err(|_| format!("JSON 解析：非法数字 {text}"))
    }

    fn boolean(&mut self) -> Result<Json, String> {
        if self.b[self.pos..].starts_with(b"true") {
            self.pos += 4;
            Ok(Json::Bool(true))
        } else if self.b[self.pos..].starts_with(b"false") {
            self.pos += 5;
            Ok(Json::Bool(false))
        } else {
            Err("JSON 解析：非法字面量".into())
        }
    }

    fn null(&mut self) -> Result<Json, String> {
        if self.b[self.pos..].starts_with(b"null") {
            self.pos += 4;
            Ok(Json::Null)
        } else {
            Err("JSON 解析：非法字面量".into())
        }
    }

    fn peek(&self) -> Option<u8> {
        self.b.get(self.pos).copied()
    }
    fn next(&mut self) -> Option<u8> {
        let c = self.b.get(self.pos).copied();
        if c.is_some() {
            self.pos += 1;
        }
        c
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let src = r#"{"name":"声场","rev":2,"ok":true,"pts":[1,2,3],"nested":{"a":null}}"#;
        let v = parse(src).unwrap();
        assert_eq!(v.str_field("name"), Some("声场"));
        assert_eq!(v.i64_field("rev"), Some(2));
        assert_eq!(v.get("ok").and_then(|x| matches!(x, Json::Bool(true)).then(|| ())), Some(()));
        assert_eq!(v.get("pts").unwrap().as_array().unwrap().len(), 3);
        let again = parse(&v.dump()).unwrap();
        assert_eq!(again.str_field("name"), Some("声场"));
    }

    #[test]
    fn escapes() {
        let v = parse(r#""a\nb\tc\"d\\e""#).unwrap();
        assert_eq!(v.as_str(), Some("a\nb\tc\"d\\e"));
    }
}
