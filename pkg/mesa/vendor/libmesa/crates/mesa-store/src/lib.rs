//! Reference persistent inbox and checkpoint in one SQLite transaction.
//! A successful commit authorizes the engine callback. No external effect is
//! performed here. See spec/12-persistent-host.md for recovery and storage limits.
mod validation;
use mesa_flow::{Change, Checkpoint, CommitRequest, CommitToken, MessageId, Outcome};
use rusqlite::{params, Connection, OpenFlags, Transaction, TransactionBehavior};
use serde::{Deserialize, Serialize};
use std::{fs::OpenOptions, path::Path, time::Duration};

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub checkpoint_bytes: usize,
    pub message_bytes: usize,
    /// Maximum rows in each application table (deliveries and settlements).
    pub delivery_records: usize,
    pub delivery_bytes: usize,
}
impl Default for Limits {
    fn default() -> Self {
        Self {
            checkpoint_bytes: 8 * 1024 * 1024,
            message_bytes: 65536,
            delivery_records: 16384,
            delivery_bytes: 64 * 1024 * 1024,
        }
    }
}
impl Limits {
    fn validate(self) -> Result<(), Error> {
        if !(1024..=64 * 1024 * 1024).contains(&self.checkpoint_bytes)
            || !(1..=1024 * 1024).contains(&self.message_bytes)
            || !(1..=16384).contains(&self.delivery_records)
            || !(self.message_bytes..=1024 * 1024 * 1024).contains(&self.delivery_bytes)
        {
            return Err(Error::Limit);
        }
        Ok(())
    }
}
#[derive(Debug)]
pub enum Error {
    Io(std::io::Error),
    Sql(rusqlite::Error),
    Json(serde_json::Error),
    InvalidState,
    Scope,
    Fenced,
    Revision,
    Transition,
    Limit,
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for Error {}
impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Self::Io(e)
    }
}
impl From<rusqlite::Error> for Error {
    fn from(e: rusqlite::Error) -> Self {
        Self::Sql(e)
    }
}
impl From<serde_json::Error> for Error {
    fn from(e: serde_json::Error) -> Self {
        Self::Json(e)
    }
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Delivery {
    pub revision: u64,
    pub id: MessageId,
    pub bytes: Vec<u8>,
    pub outcome: Outcome,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Settlement {
    pub revision: u64,
    pub id: MessageId,
    pub outcome: Outcome,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Snapshot {
    pub checkpoint: Checkpoint,
    pub deliveries: Vec<Delivery>,
    pub settlements: Vec<Settlement>,
}
/// One fenced writer. Opening a newer writer invalidates all older instances.
/// Do not recreate, copy, roll back, or delete a live database to reset a flow.
pub struct Store {
    connection: Connection,
    limits: Limits,
    incarnation: u128,
}
pub struct Prepared<'a> {
    transaction: Transaction<'a>,
    token: CommitToken,
}
impl Prepared<'_> {
    /// Return a callback token only after SQLite reports commit success.
    /// On error, withhold the callback and reopen the authoritative store.
    pub fn commit(self) -> Result<CommitToken, Error> {
        self.transaction.commit()?;
        Ok(self.token)
    }
}
impl Store {
    /// Explicit initialization, refusing every existing path. Failure can leave
    /// an incomplete file; it is never silently reused as a new flow database.
    pub fn create(
        path: impl AsRef<Path>,
        initial: &Checkpoint,
        limits: Limits,
    ) -> Result<Self, Error> {
        limits.validate()?;
        validation::initial(initial)?;
        let bytes = encode_checkpoint(initial, limits)?;
        let path = path.as_ref();
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)?
            .sync_all()?;
        #[cfg(unix)]
        std::fs::File::open(
            path.parent()
                .filter(|p| !p.as_os_str().is_empty())
                .unwrap_or(Path::new(".")),
        )?
        .sync_all()?;
        let mut connection = connect(path, false, limits)?;
        let tx = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        tx.execute_batch(
            "CREATE TABLE state (
            singleton INTEGER PRIMARY KEY CHECK(singleton=1),
            format INTEGER NOT NULL CHECK(format=2),
            tag BLOB NOT NULL CHECK(length(tag)=8),
            generation INTEGER NOT NULL CHECK(generation>0),
            revision INTEGER NOT NULL CHECK(revision>=0),
            checkpoint BLOB NOT NULL);
            CREATE TABLE deliveries (
            id BLOB PRIMARY KEY NOT NULL,
            revision INTEGER UNIQUE NOT NULL CHECK(revision>0),
            bytes BLOB NOT NULL,
            outcome INTEGER NOT NULL CHECK(outcome BETWEEN -1 AND 65535));
            CREATE TABLE settlements (
            id BLOB PRIMARY KEY NOT NULL,
            revision INTEGER UNIQUE NOT NULL CHECK(revision>0),
            outcome INTEGER NOT NULL CHECK(outcome BETWEEN -1 AND 65535));",
        )?;
        tx.execute(
            "INSERT INTO state VALUES(1,2,randomblob(8),1,0,?1)",
            [bytes],
        )?;
        let incarnation = read_incarnation(&tx)?;
        tx.commit()?;
        Ok(Self {
            connection,
            limits,
            incarnation,
        })
    }
    /// Claim a new persistent generation and load only the existing database.
    /// expected is the empty configured checkpoint, used to pin endpoint scope.
    pub fn open(
        path: impl AsRef<Path>,
        expected: &Checkpoint,
        limits: Limits,
    ) -> Result<Self, Error> {
        limits.validate()?;
        validation::initial(expected)?;
        let mut connection = connect(path.as_ref(), false, limits)?;
        let tx = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        let snapshot = read_snapshot(&tx, limits)?;
        validation::scope(expected, &snapshot.checkpoint)?;
        if tx.execute("UPDATE state SET generation=generation+1 WHERE singleton=1 AND generation<9223372036854775807",[])? != 1 {
            return Err(Error::Limit);
        }
        let incarnation = read_incarnation(&tx)?;
        tx.commit()?;
        Ok(Self {
            connection,
            limits,
            incarnation,
        })
    }
    pub fn incarnation(&self) -> u128 {
        self.incarnation
    }
    pub fn checkpoint(&self) -> Result<Checkpoint, Error> {
        read_checkpoint(&self.connection, self.limits)
    }
    /// Inspect a consistent checkpoint and inbox without taking writer ownership.
    pub fn inspect(path: impl AsRef<Path>, limits: Limits) -> Result<Snapshot, Error> {
        limits.validate()?;
        let mut connection = connect(path.as_ref(), true, limits)?;
        let tx = connection.transaction()?;
        read_snapshot(&tx, limits)
    }

    /// Stage all writes in a single IMMEDIATE transaction. Dropping this value
    /// rolls back; killing the process before commit leaves no partial effect.
    pub fn prepare(&mut self, request: &CommitRequest) -> Result<Prepared<'_>, Error> {
        if request.token.incarnation != self.incarnation {
            return Err(Error::Fenced);
        }
        let bytes = encode_checkpoint(&request.checkpoint, self.limits)?;
        let tx = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if read_incarnation(&tx)? != self.incarnation {
            return Err(Error::Fenced);
        }
        let old = read_checkpoint(&tx, self.limits)?;
        if old.revision != request.base_revision {
            return Err(Error::Revision);
        }
        validation::transition(&old, request, self.limits)?;
        if let Change::Deliver { id, bytes, outcome } = &request.change {
            let (count, total) = totals(&tx)?;
            if count >= self.limits.delivery_records
                || bytes.len() > self.limits.delivery_bytes.saturating_sub(total)
            {
                return Err(Error::Limit);
            }
            let code = match outcome {
                Outcome::Accepted => -1,
                Outcome::Rejected(code) => i64::from(*code),
            };
            tx.execute(
                "INSERT INTO deliveries(id,revision,bytes,outcome) VALUES(?1,?2,?3,?4)",
                params![
                    serde_json::to_vec(id)?,
                    revision(request.checkpoint.revision)?,
                    bytes,
                    code
                ],
            )?;
        }
        if let Change::Settle { id, outcome } = &request.change {
            let count: i64 = tx.query_row("SELECT count(*) FROM settlements", [], |r| r.get(0))?;
            if count >= self.limits.delivery_records as i64 {
                return Err(Error::Limit);
            }
            let code = match outcome {
                Outcome::Accepted => -1,
                Outcome::Rejected(code) => i64::from(*code),
            };
            tx.execute(
                "INSERT INTO settlements(id,revision,outcome) VALUES(?1,?2,?3)",
                params![
                    serde_json::to_vec(id)?,
                    revision(request.checkpoint.revision)?,
                    code
                ],
            )?;
        }
        if tx.execute(
            "UPDATE state SET checkpoint=?1,revision=?2 WHERE singleton=1 AND revision=?3",
            params![
                bytes,
                revision(request.checkpoint.revision)?,
                revision(request.base_revision)?
            ],
        )? != 1
        {
            return Err(Error::Revision);
        }
        Ok(Prepared {
            transaction: tx,
            token: request.token,
        })
    }
    pub fn commit(&mut self, request: &CommitRequest) -> Result<CommitToken, Error> {
        self.prepare(request)?.commit()
    }
}
fn revision(value: u64) -> Result<i64, Error> {
    i64::try_from(value).map_err(|_| Error::Limit)
}
fn encode_checkpoint(checkpoint: &Checkpoint, limits: Limits) -> Result<Vec<u8>, Error> {
    struct Bounded {
        bytes: Vec<u8>,
        max: usize,
        exceeded: bool,
    }
    impl std::io::Write for Bounded {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            if bytes.len() > self.max - self.bytes.len() {
                self.exceeded = true;
                return Err(std::io::Error::other("checkpoint byte limit"));
            }
            self.bytes.extend_from_slice(bytes);
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    let mut out = Bounded {
        bytes: vec![],
        max: limits.checkpoint_bytes,
        exceeded: false,
    };
    let result = serde_json::to_writer(&mut out, checkpoint);
    if out.exceeded {
        return Err(Error::Limit);
    }
    result?;
    Ok(out.bytes)
}

fn read_checkpoint(connection: &Connection, limits: Limits) -> Result<Checkpoint, Error> {
    let mut stmt =
        connection.prepare("SELECT format,revision,checkpoint FROM state WHERE singleton=1")?;
    let mut rows = stmt.query([])?;
    let row = rows.next()?.ok_or(Error::InvalidState)?;
    if row.get::<_, i64>(0)? != 2 {
        return Err(Error::InvalidState);
    }
    let bytes = row.get_ref(2)?.as_blob().map_err(|_| Error::InvalidState)?;
    if bytes.len() > limits.checkpoint_bytes {
        return Err(Error::Limit);
    }
    let checkpoint: Checkpoint = serde_json::from_slice(bytes)?;
    if revision(checkpoint.revision)? != row.get::<_, i64>(1)? {
        return Err(Error::InvalidState);
    }
    validation::state(&checkpoint, limits)?;
    Ok(checkpoint)
}
fn read_incarnation(connection: &Connection) -> Result<u128, Error> {
    let (tag, generation): (Vec<u8>, i64) = connection.query_row(
        "SELECT tag,generation FROM state WHERE singleton=1",
        [],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    let tag: [u8; 8] = tag.try_into().map_err(|_| Error::InvalidState)?;
    if generation <= 0 {
        return Err(Error::InvalidState);
    }
    Ok((u128::from(u64::from_be_bytes(tag)) << 64) | generation as u128)
}
fn outcome(code: i64) -> Result<Outcome, Error> {
    match code {
        -1 => Ok(Outcome::Accepted),
        0..=65535 => Ok(Outcome::Rejected(code as u16)),
        _ => Err(Error::InvalidState),
    }
}
fn connect(path: &Path, readonly: bool, limits: Limits) -> Result<Connection, Error> {
    let flags = if readonly {
        OpenFlags::SQLITE_OPEN_READ_ONLY
    } else {
        OpenFlags::SQLITE_OPEN_READ_WRITE
    };
    let connection = Connection::open_with_flags(path, flags | OpenFlags::SQLITE_OPEN_NO_MUTEX)?;
    connection.busy_timeout(Duration::from_millis(250))?;
    connection.set_limit(
        rusqlite::limits::Limit::SQLITE_LIMIT_LENGTH,
        (limits.checkpoint_bytes.max(limits.message_bytes) + 4096) as i32,
    )?;
    if !readonly {
        let mode: String = connection.query_row("PRAGMA journal_mode=WAL", [], |r| r.get(0))?;
        if mode != "wal" {
            return Err(Error::InvalidState);
        }
        connection.execute_batch("PRAGMA synchronous=FULL; PRAGMA fullfsync=ON; PRAGMA checkpoint_fullfsync=ON; PRAGMA foreign_keys=ON;")?;
        let sync: i64 = connection.query_row("PRAGMA synchronous", [], |r| r.get(0))?;
        if sync != 2 {
            return Err(Error::InvalidState);
        }
    }
    Ok(connection)
}

fn read_snapshot(connection: &Connection, limits: Limits) -> Result<Snapshot, Error> {
    let checkpoint = read_checkpoint(connection, limits)?;
    let (count, total) = totals(connection)?;
    if count > limits.delivery_records || total > limits.delivery_bytes {
        return Err(Error::Limit);
    }
    let mut stmt =
        connection.prepare("SELECT revision,id,bytes,outcome FROM deliveries ORDER BY revision")?;
    let mut rows = stmt.query([])?;
    let mut deliveries = vec![];
    while let Some(row) = rows.next()? {
        let id = row.get_ref(1)?.as_blob().map_err(|_| Error::InvalidState)?;
        let bytes = row.get_ref(2)?.as_blob().map_err(|_| Error::InvalidState)?;
        if id.len() > 512 || bytes.len() > limits.message_bytes {
            return Err(Error::Limit);
        }
        deliveries.push(Delivery {
            revision: u64::try_from(row.get::<_, i64>(0)?).map_err(|_| Error::InvalidState)?,
            id: serde_json::from_slice(id)?,
            bytes: bytes.to_vec(),
            outcome: outcome(row.get(3)?)?,
        });
    }
    validation::inbox(&checkpoint, &deliveries)?;
    let count: i64 = connection.query_row("SELECT count(*) FROM settlements", [], |r| r.get(0))?;
    if count < 0 || count > limits.delivery_records as i64 {
        return Err(Error::Limit);
    }
    let mut stmt =
        connection.prepare("SELECT revision,id,outcome FROM settlements ORDER BY revision")?;
    let mut rows = stmt.query([])?;
    let mut settlements = vec![];
    while let Some(row) = rows.next()? {
        let id = row.get_ref(1)?.as_blob().map_err(|_| Error::InvalidState)?;
        if id.len() > 512 {
            return Err(Error::Limit);
        }
        settlements.push(Settlement {
            revision: u64::try_from(row.get::<_, i64>(0)?).map_err(|_| Error::InvalidState)?,
            id: serde_json::from_slice(id)?,
            outcome: outcome(row.get(2)?)?,
        });
    }
    validation::settlements(&checkpoint, &settlements)?;
    Ok(Snapshot {
        checkpoint,
        deliveries,
        settlements,
    })
}

fn totals(connection: &Connection) -> Result<(usize, usize), Error> {
    let (count, total): (i64, i64) = connection.query_row(
        "SELECT count(*),coalesce(sum(length(bytes)),0) FROM deliveries",
        [],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    Ok((
        usize::try_from(count).map_err(|_| Error::InvalidState)?,
        usize::try_from(total).map_err(|_| Error::InvalidState)?,
    ))
}
