//! libsqlite3 的最小安全封装：只声明本工程用到的 C API。
//! 所有 unsafe 都收敛在本文件内，业务代码只看到 `Db` / `Stmt` 两个安全类型。
//! 链接由 build.rs 的 `cargo:rustc-link-lib=sqlite3` 统一负责。
#![allow(dead_code)]

use std::ffi::{c_char, c_int, CStr, CString};
use std::ptr;

use crate::error::{AppError, AppResult};

const SQLITE_OK: c_int = 0;
const SQLITE_ROW: c_int = 100;
const SQLITE_DONE: c_int = 101;
const SQLITE_OPEN_READWRITE: c_int = 0x00000002;
const SQLITE_OPEN_CREATE: c_int = 0x00000004;
const SQLITE_INTEGER: c_int = 1;
const SQLITE_FLOAT: c_int = 2;
const SQLITE_TEXT: c_int = 3;
const SQLITE_BLOB: c_int = 4;
const SQLITE_NULL: c_int = 5;

/// SQLITE_TRANSIENT：告诉 SQLite 自己复制绑定数据。
const SQLITE_TRANSIENT: Option<unsafe extern "C" fn(*mut std::ffi::c_void)> =
    Some(unsafe { std::mem::transmute(-1isize) });

type Sqlite3 = std::ffi::c_void;
type Sqlite3Stmt = std::ffi::c_void;

extern "C" {
    fn sqlite3_open_v2(
        filename: *const c_char,
        db: *mut *mut Sqlite3,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int;
    fn sqlite3_close_v2(db: *mut Sqlite3) -> c_int;
    fn sqlite3_exec(
        db: *mut Sqlite3,
        sql: *const c_char,
        cb: Option<unsafe extern "C" fn(*mut std::ffi::c_void, c_int, *mut *mut c_char, *mut *mut c_char) -> c_int>,
        arg: *mut std::ffi::c_void,
        errmsg: *mut *mut c_char,
    ) -> c_int;
    fn sqlite3_prepare_v2(
        db: *mut Sqlite3,
        sql: *const c_char,
        nbyte: c_int,
        stmt: *mut *mut Sqlite3Stmt,
        tail: *mut *const c_char,
    ) -> c_int;
    fn sqlite3_step(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_finalize(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_reset(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_bind_null(stmt: *mut Sqlite3Stmt, idx: c_int) -> c_int;
    fn sqlite3_bind_int64(stmt: *mut Sqlite3Stmt, idx: c_int, val: i64) -> c_int;
    fn sqlite3_bind_double(stmt: *mut Sqlite3Stmt, idx: c_int, val: f64) -> c_int;
    fn sqlite3_bind_text(
        stmt: *mut Sqlite3Stmt,
        idx: c_int,
        text: *const c_char,
        n: c_int,
        destructor: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
    ) -> c_int;
    fn sqlite3_bind_blob(
        stmt: *mut Sqlite3Stmt,
        idx: c_int,
        data: *const std::ffi::c_void,
        n: c_int,
        destructor: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
    ) -> c_int;
    fn sqlite3_column_type(stmt: *mut Sqlite3Stmt, col: c_int) -> c_int;
    fn sqlite3_column_int64(stmt: *mut Sqlite3Stmt, col: c_int) -> i64;
    fn sqlite3_column_double(stmt: *mut Sqlite3Stmt, col: c_int) -> f64;
    fn sqlite3_column_text(stmt: *mut Sqlite3Stmt, col: c_int) -> *const c_char;
    fn sqlite3_column_blob(stmt: *mut Sqlite3Stmt, col: c_int) -> *const std::ffi::c_void;
    fn sqlite3_column_bytes(stmt: *mut Sqlite3Stmt, col: c_int) -> c_int;
    fn sqlite3_last_insert_rowid(db: *mut Sqlite3) -> i64;
    fn sqlite3_changes(db: *mut Sqlite3) -> c_int;
    fn sqlite3_errmsg(db: *mut Sqlite3) -> *const c_char;
    fn sqlite3_free(ptr: *mut std::ffi::c_void);
    fn sqlite3_busy_timeout(db: *mut Sqlite3, ms: c_int) -> c_int;
}

pub struct Db {
    handle: *mut Sqlite3,
}

// 本服务在单线程运行时内使用；发送到 accept 线程需要 Send。
unsafe impl Send for Db {}

impl Db {
    pub fn open(path: &str) -> AppResult<Db> {
        let cpath = CString::new(path).map_err(|e| AppError::Sqlite(e.to_string()))?;
        let mut handle: *mut Sqlite3 = ptr::null_mut();
        let rc = unsafe {
            sqlite3_open_v2(
                cpath.as_ptr(),
                &mut handle,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                ptr::null(),
            )
        };
        if rc != SQLITE_OK {
            let msg = if !handle.is_null() {
                unsafe { cstr(sqlite3_errmsg(handle)) }
            } else {
                format!("sqlite3_open_v2 返回 {rc}")
            };
            if !handle.is_null() {
                unsafe {
                    sqlite3_close_v2(handle);
                }
            }
            return Err(AppError::Sqlite(msg));
        }
        let db = Db { handle };
        db.exec("PRAGMA journal_mode=WAL;")?;
        db.exec("PRAGMA foreign_keys=ON;")?;
        unsafe {
            sqlite3_busy_timeout(handle, 5000);
        }
        Ok(db)
    }

    pub fn exec(&self, sql: &str) -> AppResult<()> {
        let csql = CString::new(sql).map_err(|e| AppError::Sqlite(e.to_string()))?;
        let mut err: *mut c_char = ptr::null_mut();
        let rc = unsafe {
            sqlite3_exec(
                self.handle,
                csql.as_ptr(),
                None,
                ptr::null_mut(),
                &mut err,
            )
        };
        if rc != SQLITE_OK {
            let msg = if !err.is_null() {
                let m = unsafe { cstr(err) };
                // sqlite3_exec 的错误信息必须用 sqlite3_free 释放
                unsafe {
                    sqlite3_free(err as *mut std::ffi::c_void);
                }
                m
            } else {
                unsafe { cstr(sqlite3_errmsg(self.handle)) }
            };
            return Err(AppError::Sqlite(msg));
        }
        Ok(())
    }

    pub fn prepare(&self, sql: &str) -> AppResult<Stmt> {
        let csql = CString::new(sql).map_err(|e| AppError::Sqlite(e.to_string()))?;
        let mut stmt: *mut Sqlite3Stmt = ptr::null_mut();
        let rc = unsafe {
            sqlite3_prepare_v2(
                self.handle,
                csql.as_ptr(),
                -1,
                &mut stmt,
                ptr::null_mut(),
            )
        };
        if rc != SQLITE_OK {
            return Err(AppError::Sqlite(format!(
                "预编译失败 [{sql}]: {}",
                unsafe { cstr(sqlite3_errmsg(self.handle)) }
            )));
        }
        Ok(Stmt {
            db: self.handle,
            stmt,
        })
    }

    pub fn last_insert_rowid(&self) -> i64 {
        unsafe { sqlite3_last_insert_rowid(self.handle) }
    }

    pub fn changes(&self) -> i64 {
        unsafe { sqlite3_changes(self.handle) as i64 }
    }

    /// 在立即事务内执行闭包；闭包返回 Err 时自动 ROLLBACK。
    pub fn transaction<T, F>(&mut self, f: F) -> AppResult<T>
    where
        F: FnOnce(&mut Db) -> AppResult<T>,
    {
        self.exec("BEGIN IMMEDIATE;")?;
        match f(self) {
            Ok(v) => {
                self.exec("COMMIT;")?;
                Ok(v)
            }
            Err(e) => {
                // 回滚失败也不能掩盖原始错误
                let _ = self.exec("ROLLBACK;");
                Err(e)
            }
        }
    }
}

impl Drop for Db {
    fn drop(&mut self) {
        unsafe {
            sqlite3_close_v2(self.handle);
        }
    }
}

unsafe fn cstr(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

pub struct Stmt {
    db: *mut Sqlite3,
    stmt: *mut Sqlite3Stmt,
}

impl Stmt {
    pub fn bind_text(&mut self, idx: c_int, val: &str) -> AppResult<&mut Self> {
        let rc = unsafe {
            sqlite3_bind_text(
                self.stmt,
                idx,
                val.as_ptr() as *const c_char,
                val.len() as c_int,
                SQLITE_TRANSIENT,
            )
        };
        if rc == SQLITE_OK {
            Ok(self)
        } else {
            self.err(rc)
        }
    }

    pub fn bind_i64(&mut self, idx: c_int, val: i64) -> AppResult<&mut Self> {
        let rc = unsafe { sqlite3_bind_int64(self.stmt, idx, val) };
        if rc == SQLITE_OK {
            Ok(self)
        } else {
            self.err(rc)
        }
    }

    pub fn bind_f64(&mut self, idx: c_int, val: f64) -> AppResult<&mut Self> {
        let rc = unsafe { sqlite3_bind_double(self.stmt, idx, val) };
        if rc == SQLITE_OK {
            Ok(self)
        } else {
            self.err(rc)
        }
    }

    pub fn bind_null(&mut self, idx: c_int) -> AppResult<&mut Self> {
        let rc = unsafe { sqlite3_bind_null(self.stmt, idx) };
        if rc == SQLITE_OK {
            Ok(self)
        } else {
            self.err(rc)
        }
    }

    pub fn bind_blob(&mut self, idx: c_int, data: &[u8]) -> AppResult<&mut Self> {
        let rc = unsafe {
            sqlite3_bind_blob(
                self.stmt,
                idx,
                data.as_ptr() as *const std::ffi::c_void,
                data.len() as c_int,
                SQLITE_TRANSIENT,
            )
        };
        if rc == SQLITE_OK {
            Ok(self)
        } else {
            self.err(rc)
        }
    }

    fn err<T>(&self, rc: c_int) -> AppResult<T> {
        Err(AppError::Sqlite(format!(
            "SQLite 调用返回 {rc}: {}",
            unsafe { cstr(sqlite3_errmsg(self.db)) }
        )))
    }

    /// 推进一行；返回 false 表示 DONE。
    pub fn next(&mut self) -> AppResult<bool> {
        let rc = unsafe { sqlite3_step(self.stmt) };
        match rc {
            SQLITE_ROW => Ok(true),
            SQLITE_DONE => Ok(false),
            _ => Err(AppError::Sqlite(format!(
                "执行失败: {}",
                unsafe { cstr(sqlite3_errmsg(self.db)) }
            ))),
        }
    }

    pub fn column_i64(&self, col: c_int) -> i64 {
        unsafe { sqlite3_column_int64(self.stmt, col) }
    }

    pub fn column_f64(&self, col: c_int) -> f64 {
        unsafe { sqlite3_column_double(self.stmt, col) }
    }

    pub fn column_text(&self, col: c_int) -> String {
        unsafe {
            let p = sqlite3_column_text(self.stmt, col);
            cstr(p)
        }
    }

    pub fn column_blob(&self, col: c_int) -> Vec<u8> {
        unsafe {
            let n = sqlite3_column_bytes(self.stmt, col) as usize;
            let p = sqlite3_column_blob(self.stmt, col);
            if p.is_null() || n == 0 {
                Vec::new()
            } else {
                std::slice::from_raw_parts(p as *const u8, n).to_vec()
            }
        }
    }

    pub fn column_is_null(&self, col: c_int) -> bool {
        unsafe { sqlite3_column_type(self.stmt, col) == SQLITE_NULL }
    }

    pub fn column_type(&self, col: c_int) -> c_int {
        unsafe { sqlite3_column_type(self.stmt, col) }
    }

    pub fn reset(&mut self) -> AppResult<()> {
        let rc = unsafe { sqlite3_reset(self.stmt) };
        if rc == SQLITE_OK {
            Ok(())
        } else {
            Err(AppError::Sqlite("reset statement 失败".into()))
        }
    }
}

impl Drop for Stmt {
    fn drop(&mut self) {
        unsafe {
            sqlite3_finalize(self.stmt);
        }
    }
}
