//! Bounded lossless PLAN rows/nats. Byte strings use PLAN bars (a terminal 1).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Value {
    Nat(Vec<u8>),
    Row(Vec<Value>),
}
pub type Result<T> = std::result::Result<T, String>;
pub const LIMIT: usize = 16 * 1024 * 1024;
impl Value {
    pub fn num(n: u128) -> Self {
        let mut b = n.to_le_bytes().to_vec();
        while b.last() == Some(&0) {
            b.pop();
        }
        Self::Nat(b)
    }
    pub fn bytes(b: impl AsRef<[u8]>) -> Self {
        let mut b = b.as_ref().to_vec();
        b.push(1);
        Self::Nat(b)
    }
    pub fn text(s: impl AsRef<str>) -> Self {
        Self::bytes(s.as_ref().as_bytes())
    }
    pub fn row(v: impl IntoIterator<Item = Value>) -> Self {
        Self::Row(v.into_iter().collect())
    }
    pub fn list(&self) -> Result<&[Value]> {
        match self {
            Self::Row(v) => Ok(v),
            Self::Nat(v) if v.is_empty() => Ok(&[]),
            _ => Err("expected row".into()),
        }
    }
    pub fn at(&self, i: usize) -> Result<&Value> {
        self.list()?
            .get(i)
            .ok_or_else(|| format!("missing field {i}"))
    }
    pub fn n(&self) -> Result<u128> {
        match self {
            Self::Nat(b) if b.len() <= 16 => {
                let mut a = [0; 16];
                a[..b.len()].copy_from_slice(b);
                Ok(u128::from_le_bytes(a))
            }
            _ => Err("expected u128".into()),
        }
    }
    pub fn u(&self) -> Result<u64> {
        self.n()?.try_into().map_err(|_| "u64 overflow".into())
    }
    pub fn size(&self) -> Result<usize> {
        self.n()?.try_into().map_err(|_| "usize overflow".into())
    }
    pub fn bool(&self) -> Result<bool> {
        match self.n()? {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err("expected boolean".into()),
        }
    }
    pub fn bar(&self) -> Result<&[u8]> {
        match self {
            Self::Nat(b) if b.last() == Some(&1) => Ok(&b[..b.len() - 1]),
            _ => Err("expected byte bar".into()),
        }
    }
    pub fn str(&self) -> Result<&str> {
        std::str::from_utf8(self.bar()?).map_err(|_| "invalid UTF-8".into())
    }
    pub fn fixed<const N: usize>(&self) -> Result<[u8; N]> {
        self.bar()?
            .try_into()
            .map_err(|_| format!("expected {N} bytes"))
    }
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        fn read<'a>(b: &mut &'a [u8], n: usize) -> Result<&'a [u8]> {
            if n > b.len() {
                return Err("truncated argument".into());
            }
            let (a, r) = b.split_at(n);
            *b = r;
            Ok(a)
        }
        fn go(b: &mut &[u8], depth: usize, nodes: &mut usize) -> Result<Value> {
            if depth > 64 || *nodes == 0 {
                return Err("argument limit".into());
            }
            *nodes -= 1;
            let tag = read(b, 1)?[0];
            let n = u32::from_le_bytes(read(b, 4)?.try_into().unwrap()) as usize;
            match tag {
                0 => {
                    let v = read(b, n)?.to_vec();
                    if v.last() == Some(&0) {
                        return Err("noncanonical nat".into());
                    }
                    Ok(Value::Nat(v))
                }
                1 => {
                    if n > *nodes {
                        return Err("row limit".into());
                    }
                    let mut v = Vec::with_capacity(n);
                    for _ in 0..n {
                        v.push(go(b, depth + 1, nodes)?)
                    }
                    Ok(Value::Row(v))
                }
                _ => Err("invalid value tag".into()),
            }
        }
        if bytes.len() > LIMIT {
            return Err("argument size limit".into());
        }
        let mut b = bytes;
        let v = go(&mut b, 0, &mut 1_000_000)?;
        if !b.is_empty() {
            return Err("trailing argument bytes".into());
        }
        Ok(v)
    }
    pub fn encode(&self) -> Result<Vec<u8>> {
        fn go(v: &Value, b: &mut Vec<u8>, depth: usize) -> Result<()> {
            if depth > 64 {
                return Err("result depth limit".into());
            }
            let n = match v {
                Value::Nat(v) => v.len(),
                Value::Row(v) => v.len(),
            };
            if n > LIMIT || b.len().saturating_add(n).saturating_add(5) > LIMIT {
                return Err("result size limit".into());
            }
            b.push(if matches!(v, Value::Nat(_)) { 0 } else { 1 });
            b.extend_from_slice(&(n as u32).to_le_bytes());
            match v {
                Value::Nat(v) => b.extend_from_slice(v),
                Value::Row(v) => {
                    for x in v {
                        go(x, b, depth + 1)?
                    }
                }
            }
            Ok(())
        }
        let mut b = Vec::new();
        go(self, &mut b, 0)?;
        Ok(b)
    }
}
pub fn err(e: impl std::fmt::Debug) -> String {
    format!("{e:?}")
}
pub fn nums(v: impl IntoIterator<Item = u128>) -> Value {
    Value::row(v.into_iter().map(Value::num))
}
