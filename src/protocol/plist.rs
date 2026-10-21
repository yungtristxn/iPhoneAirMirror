// Preflight binary-plist object graph before invoking the maintained plist reader.
// This rejects cycles, excessive nesting, aliases with exponential expansion and oversized counts.
use anyhow::{Result, bail, ensure};
use plist::{Dictionary, Value};
fn uint(data: &[u8]) -> Result<usize> {
    ensure!(!data.is_empty() && data.len() <= 8, "plist integer width");
    let mut n = 0u64;
    for b in data {
        n = (n << 8) | u64::from(*b);
    }
    Ok(usize::try_from(n)?)
}
pub fn decode(data: &[u8]) -> Result<Value> {
    ensure!(
        (40..=65536).contains(&data.len()) && &data[..8] == b"bplist00",
        "binary plist required"
    );
    let t = &data[data.len() - 32..];
    let os = t[6] as usize;
    let rs = t[7] as usize;
    ensure!(
        matches!(os, 1 | 2 | 4 | 8) && matches!(rs, 1 | 2 | 4 | 8),
        "plist widths"
    );
    let count = uint(&t[8..16])?;
    let root = uint(&t[16..24])?;
    let table = uint(&t[24..])?;
    ensure!(
        (1..=1024).contains(&count)
            && root < count
            && table >= 8
            && table.checked_add(count * os) == Some(data.len() - 32),
        "plist table"
    );
    let offsets: Vec<usize> = (0..count)
        .map(|i| uint(&data[table + i * os..table + (i + 1) * os]))
        .collect::<Result<_>>()?;
    ensure!(
        offsets.iter().all(|o| *o >= 8 && *o < table),
        "plist offsets"
    );
    #[allow(clippy::too_many_arguments)] // Explicit recursion budgets stay beside the input graph.
    fn visit(
        index: usize,
        depth: usize,
        offsets: &[usize],
        rs: usize,
        data: &[u8],
        path: &mut Vec<usize>,
        budget: &mut usize,
        byte_budget: &mut usize,
    ) -> Result<()> {
        ensure!(
            depth <= 16 && *budget > 0 && !path.contains(&index),
            "plist graph budget or cycle"
        );
        *budget -= 1;
        path.push(index);
        let mut p = offsets[index];
        let tag = data[p];
        p += 1;
        let kind = tag >> 4;
        let mut len = (tag & 15) as usize;
        if matches!(kind, 4 | 5 | 6 | 10 | 13) && len == 15 {
            let marker = *data
                .get(p)
                .ok_or_else(|| anyhow::anyhow!("plist length marker"))?;
            p += 1;
            ensure!(marker >> 4 == 1 && marker & 15 <= 3, "plist length integer");
            let width = 1usize << (marker & 15);
            len = uint(
                data.get(p..p + width)
                    .ok_or_else(|| anyhow::anyhow!("plist length"))?,
            )?;
            p += width;
        }
        let bytes = match kind {
            0 => {
                ensure!(matches!(tag, 0 | 8 | 9 | 15), "plist primitive");
                0
            }
            1 => {
                // Binary plists may encode a u64 with its high bit set as a
                // zero-extended 16-byte integer. The reader still bounds it to u64.
                ensure!(len <= 4, "plist integer width");
                1 << len
            }
            2 => {
                ensure!(matches!(len, 2 | 3), "plist real width");
                1 << len
            }
            3 => {
                ensure!(tag == 0x33, "plist date");
                8
            }
            4 | 5 => {
                ensure!(len <= 65536, "plist data");
                len
            }
            6 => {
                ensure!(len <= 32768, "plist string");
                len * 2
            }
            8 => {
                ensure!(len < 8, "plist UID");
                len + 1
            }
            10 | 13 => {
                ensure!(len <= 256, "plist collection");
                len * rs * if kind == 13 { 2 } else { 1 }
            }
            _ => bail!("unsupported plist object"),
        };
        let value = data
            .get(p..p + bytes)
            .ok_or_else(|| anyhow::anyhow!("truncated plist object"))?;
        if kind == 1 && len == 4 {
            ensure!(value[..8] == [0; 8], "plist integer exceeds u64");
        }
        let cost = bytes * 3 + 64;
        ensure!(cost <= *byte_budget, "expanded plist allocation budget");
        *byte_budget -= cost;
        if matches!(kind, 10 | 13) {
            for r in value.chunks_exact(rs) {
                let n = uint(r)?;
                ensure!(n < offsets.len(), "plist reference");
                visit(n, depth + 1, offsets, rs, data, path, budget, byte_budget)?;
            }
        }
        path.pop();
        Ok(())
    }
    visit(
        root,
        0,
        &offsets,
        rs,
        &data[..table],
        &mut Vec::new(),
        &mut 4096,
        &mut (512 * 1024),
    )?;
    Ok(Value::from_reader(std::io::Cursor::new(data))?)
}
pub fn encode(value: &Value) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    value.to_writer_binary(&mut out)?;
    Ok(out)
}
pub fn dict(items: impl IntoIterator<Item = (&'static str, Value)>) -> Value {
    Value::Dictionary(
        items
            .into_iter()
            .map(|(k, v)| (k.to_string(), v))
            .collect::<Dictionary>(),
    )
}
pub fn number(n: u64) -> Value {
    Value::Integer(n.into())
}
pub fn get<'a>(v: &'a Value, k: &str) -> Result<&'a Value> {
    v.as_dictionary()
        .and_then(|d| d.get(k))
        .ok_or_else(|| anyhow::anyhow!("missing field {k}"))
}
pub fn num(v: &Value, k: &str) -> Result<u64> {
    get(v, k)?
        .as_unsigned_integer()
        .ok_or_else(|| anyhow::anyhow!("invalid integer {k}"))
}
/// The stream ID is an opaque 64-bit bit pattern. Apple can encode it as
/// signed i64; preserve those bits for the decimal AES key/IV derivation.
/// Numeric fields such as lengths, ports and counts must continue to use num().
pub fn stream_connection_id(v: &Value) -> Result<u64> {
    let id = get(v, "streamConnectionID")?;
    id.as_unsigned_integer()
        .or_else(|| id.as_signed_integer().map(|n| n as u64))
        .ok_or_else(|| anyhow::anyhow!("invalid stream connection identifier"))
}
pub fn bytes<const N: usize>(v: &Value, k: &str) -> Result<[u8; N]> {
    get(v, k)?
        .as_data()
        .and_then(|d| d.try_into().ok())
        .ok_or_else(|| anyhow::anyhow!("invalid fixed field {k}"))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn roundtrip() {
        let v = dict([("port", number(1234)), ("data", Value::Data(vec![3; 72]))]);
        assert_eq!(decode(&encode(&v).unwrap()).unwrap(), v);
    }
    #[test]
    fn stream_identifier_preserves_all_64_bits() {
        for value in [0u64, 1, i64::MAX as u64, 1u64 << 63, u64::MAX] {
            for integer in [
                Value::Integer(value.into()),
                Value::Integer((value as i64).into()),
            ] {
                let decoded =
                    decode(&encode(&dict([("streamConnectionID", integer)])).unwrap()).unwrap();
                assert_eq!(stream_connection_id(&decoded).unwrap(), value);
            }
        }
    }
    #[test]
    fn negative_numbers_remain_invalid_for_numeric_fields() {
        let v =
            decode(&encode(&dict([("port", Value::Integer((-1i64).into()))])).unwrap()).unwrap();
        assert!(num(&v, "port").is_err());
        for v in [
            Value::Real(12.0),
            Value::Data(vec![0; 8]),
            Value::String("123".into()),
        ] {
            assert!(stream_connection_id(&dict([("streamConnectionID", v)])).is_err());
        }
    }
    #[test]
    fn all_truncations_rejected() {
        let v = encode(&dict([("a", number(1))])).unwrap();
        for n in 0..v.len() {
            assert!(decode(&v[..n]).is_err());
        }
    }
    #[test]
    fn hostile_trailer() {
        let mut v = encode(&dict([("a", number(1))])).unwrap();
        let n = v.len();
        v[n - 24..n - 16].fill(255);
        assert!(decode(&v).is_err());
    }
    #[test]
    fn deep_graph_rejected() {
        let mut v = number(1);
        for _ in 0..18 {
            v = Value::Array(vec![v]);
        }
        assert!(decode(&encode(&v).unwrap()).is_err());
    }
    #[test]
    fn cyclic_reference_rejected() {
        let mut v = b"bplist00".to_vec();
        v.extend_from_slice(&[0xa1, 0, 8]);
        let mut t = [0; 32];
        t[6] = 1;
        t[7] = 1;
        t[15] = 1;
        t[31] = 10;
        v.extend_from_slice(&t);
        assert!(decode(&v).is_err());
    }
    #[test]
    fn shared_blob_expansion_is_bounded() {
        let mut v = b"bplist00".to_vec();
        v.extend_from_slice(&[0xaf, 0x11, 1, 0]);
        v.extend_from_slice(&[1; 256]);
        let data_offset = v.len() as u32;
        v.extend_from_slice(&[0x4f, 0x12, 0, 0, 0x80, 0]);
        v.resize(v.len() + 32768, 7);
        let table = v.len() as u64;
        v.extend_from_slice(&8u32.to_be_bytes());
        v.extend_from_slice(&data_offset.to_be_bytes());
        let mut t = [0; 32];
        t[6] = 4;
        t[7] = 1;
        t[15] = 2;
        t[24..].copy_from_slice(&table.to_be_bytes());
        v.extend_from_slice(&t);
        assert!(
            decode(&v)
                .unwrap_err()
                .to_string()
                .contains("allocation budget")
        );
    }
    #[test]
    fn deterministic_mutated_plists_do_not_panic() {
        let seed = encode(&dict([
            ("x", Value::Data(vec![3; 72])),
            ("port", number(7000)),
        ]))
        .unwrap();
        let mut state = 0x12345678u32;
        for _ in 0..4096 {
            let mut v = seed.clone();
            for _ in 0..4 {
                state = state.wrapping_mul(1664525).wrapping_add(1013904223);
                let p = state as usize % v.len();
                v[p] = (state >> 24) as u8;
            }
            let _ = decode(&v);
        }
    }
}
