//! 统一错误类型。整个工程不依赖第三方 crate，错误也只用标准库表达。
use std::fmt;

#[derive(Debug)]
pub enum AppError {
    Io(String),
    Sqlite(String),
    Http(String),
    Json(String),
    Conflict(String),
    Validation(String),
    NotFound(String),
}

impl fmt::Display for AppError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AppError::Io(m) => write!(f, "IO 错误: {m}"),
            AppError::Sqlite(m) => write!(f, "SQLite 错误: {m}"),
            AppError::Http(m) => write!(f, "HTTP 错误: {m}"),
            AppError::Json(m) => write!(f, "JSON 错误: {m}"),
            AppError::Conflict(m) => write!(f, "版本冲突: {m}"),
            AppError::Validation(m) => write!(f, "参数校验失败: {m}"),
            AppError::NotFound(m) => write!(f, "未找到: {m}"),
        }
    }
}

impl std::error::Error for AppError {}

impl From<std::io::Error> for AppError {
    fn from(e: std::io::Error) -> Self {
        AppError::Io(e.to_string())
    }
}

impl From<std::str::Utf8Error> for AppError {
    fn from(e: std::str::Utf8Error) -> Self {
        AppError::Json(format!("UTF-8 转换失败: {e}"))
    }
}

impl From<std::num::ParseIntError> for AppError {
    fn from(e: std::num::ParseIntError) -> Self {
        AppError::Validation(format!("整数解析失败: {e}"))
    }
}

pub type AppResult<T> = Result<T, AppError>;
