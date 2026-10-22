//! Bounded transient HAP pair-setup. No persistent peers or authentication fallback.
//! Wire reference: Shairport Sync b972e900d9e68d3cb29745c7e46cf9d74abb6260,
//! pair_ap/pair_homekit.c. SRP arithmetic lives in the maintained library adapter.
use super::{hap_srp::SrpSetup, secure};
use anyhow::{Result, bail, ensure};
use std::{
    collections::BTreeMap,
    time::{Duration, Instant},
};
use zeroize::Zeroizing;

const MAX_TLV: usize = 1024;
const SETUP_DEADLINE: Duration = Duration::from_secs(10);

#[derive(Clone)]
pub struct SessionSecret(Zeroizing<[u8; 64]>);
impl SessionSecret {
    pub fn bytes(&self) -> &[u8; 64] {
        &self.0
    }
    pub fn stream_key(&self, id: u64) -> Result<[u8; 32]> {
        secure::derive_key(
            self.bytes(),
            format!("DataStream-Salt{id}").as_bytes(),
            b"DataStream-Output-Encryption-Key",
        )
    }
}

enum State {
    Idle,
    Pending {
        setup: Box<SrpSetup>,
        started: Instant,
    },
    Verified(SessionSecret),
    Failed,
}
pub struct Pairing {
    state: State,
}
impl Default for Pairing {
    fn default() -> Self {
        Self { state: State::Idle }
    }
}
impl Pairing {
    pub fn verified(&self) -> Result<&SessionSecret> {
        match &self.state {
            State::Verified(secret) => Ok(secret),
            _ => bail!("HAP proof required"),
        }
    }
    /// M4 remains plaintext. The caller upgrades the transport only after the
    /// complete successful response is flushed, retaining buffered next-record bytes.
    pub fn setup(&mut self, body: &[u8]) -> Result<(Vec<u8>, Option<SessionSecret>)> {
        let previous = std::mem::replace(&mut self.state, State::Failed);
        let fields = decode(body)?;
        match previous {
            State::Idle => {
                exact_fields(&fields, &[0, 6, 19])?;
                ensure!(
                    field(&fields, 0)? == [0] && field(&fields, 6)? == [1],
                    "HAP setup method/state"
                );
                let flags = field(&fields, 19)?;
                ensure!(!flags.is_empty() && flags.len() <= 4, "HAP flags length");
                let mut bits = [0u8; 4];
                bits[..flags.len()].copy_from_slice(flags);
                ensure!(
                    u32::from_le_bytes(bits) == 0x10,
                    "only transient HAP setup is supported"
                );
                let setup = Box::new(SrpSetup::new()?);
                let mut reply = Vec::with_capacity(410);
                encode(&mut reply, 6, &[2])?;
                encode(&mut reply, 2, setup.salt())?;
                encode(&mut reply, 3, setup.public())?;
                self.state = State::Pending {
                    setup,
                    started: Instant::now(),
                };
                Ok((reply, None))
            }
            State::Pending { setup, started } => {
                ensure!(started.elapsed() <= SETUP_DEADLINE, "HAP setup expired");
                exact_fields(&fields, &[3, 4, 6])?;
                ensure!(field(&fields, 6)? == [3], "HAP proof state");
                let (key, server_proof) = setup.verify(field(&fields, 3)?, field(&fields, 4)?)?;
                let secret = SessionSecret(Zeroizing::new(key));
                let mut reply = Vec::with_capacity(69);
                encode(&mut reply, 6, &[4])?;
                encode(&mut reply, 4, &server_proof)?;
                self.state = State::Verified(secret.clone());
                Ok((reply, Some(secret)))
            }
            State::Verified(_) | State::Failed => bail!("HAP pairing sequence closed"),
        }
    }
}
fn field(fields: &BTreeMap<u8, Vec<u8>>, tag: u8) -> Result<&[u8]> {
    fields
        .get(&tag)
        .map(Vec::as_slice)
        .ok_or_else(|| anyhow::anyhow!("missing HAP field"))
}
fn exact_fields(fields: &BTreeMap<u8, Vec<u8>>, tags: &[u8]) -> Result<()> {
    ensure!(
        fields.len() == tags.len() && tags.iter().all(|tag| fields.contains_key(tag)),
        "unexpected HAP fields"
    );
    Ok(())
}
fn decode(data: &[u8]) -> Result<BTreeMap<u8, Vec<u8>>> {
    ensure!(!data.is_empty() && data.len() <= MAX_TLV, "HAP TLV budget");
    let mut fields: BTreeMap<u8, Vec<u8>> = BTreeMap::new();
    let mut offset = 0;
    let mut previous = None;
    let mut previous_len = 0;
    while offset < data.len() {
        ensure!(data.len() - offset >= 2, "truncated HAP TLV header");
        let tag = data[offset];
        let len = usize::from(data[offset + 1]);
        offset += 2;
        ensure!(
            len != 0 && len <= data.len() - offset,
            "truncated or empty HAP TLV"
        );
        ensure!(matches!(tag, 0 | 3 | 4 | 6 | 19), "unsupported HAP field");
        if let Some(value) = fields.get_mut(&tag) {
            ensure!(
                previous == Some(tag) && previous_len == 255 && value.len() + len <= 384,
                "duplicate HAP TLV"
            );
            value.extend_from_slice(&data[offset..offset + len]);
        } else {
            ensure!(fields.len() < 5, "HAP field count");
            fields.insert(tag, data[offset..offset + len].to_vec());
        }
        previous = Some(tag);
        previous_len = len;
        offset += len;
    }
    Ok(fields)
}
fn encode(out: &mut Vec<u8>, tag: u8, value: &[u8]) -> Result<()> {
    ensure!(
        !value.is_empty() && out.len() + value.len() + 2 * value.len().div_ceil(255) <= MAX_TLV,
        "HAP response budget"
    );
    for part in value.chunks(255) {
        out.extend_from_slice(&[tag, part.len() as u8]);
        out.extend_from_slice(part);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn tlv_rejects_truncation_duplicates_and_unbounded_fragments() {
        for bad in [
            vec![],
            vec![6],
            vec![6, 2, 1],
            vec![6, 0],
            vec![6, 1, 1, 6, 1, 1],
            vec![99, 1, 1],
            vec![0; 1025],
        ] {
            assert!(decode(&bad).is_err());
        }
        let mut public = vec![3, 255];
        public.extend_from_slice(&[9; 255]);
        public.extend_from_slice(&[3, 129]);
        public.extend_from_slice(&[9; 129]);
        assert_eq!(decode(&public).unwrap()[&3].len(), 384);
        public.extend_from_slice(&[3, 1, 9]);
        assert!(decode(&public).is_err());
        let mut separated = vec![3, 255];
        separated.extend_from_slice(&[9; 255]);
        separated.extend_from_slice(&[6, 1, 3, 3, 1, 9]);
        assert!(decode(&separated).is_err());
    }
    #[test]
    fn no_key_before_proof_and_failure_never_restarts_or_downgrades() {
        let mut pairing = Pairing::default();
        assert!(pairing.verified().is_err());
        assert!(pairing.setup(&[6, 1, 3, 3, 1, 1, 4, 1, 1]).is_err());
        assert!(pairing.setup(&[0, 1, 0, 6, 1, 1, 19, 1, 16]).is_err());
        assert!(pairing.verified().is_err());
        let mut pairing = Pairing::default();
        assert!(pairing.setup(&[0, 1, 0, 6, 1, 1, 19, 1, 0]).is_err());
        assert!(pairing.verified().is_err());
    }
}
