use aes::cipher::{KeyIvInit, StreamCipher};
use anyhow::{Result, bail, ensure};
use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};
use rand::{RngCore, rngs::OsRng};
use sha2::{Digest, Sha512};
use x25519_dalek::{PublicKey, StaticSecret};
use zeroize::Zeroize;
pub type AesCtr = ctr::Ctr128BE<aes::Aes128>;
pub fn hash16(a: &[u8], b: &[u8]) -> [u8; 16] {
    let mut h = Sha512::new();
    h.update(a);
    h.update(b);
    h.finalize()[..16].try_into().unwrap()
}
pub fn video_cipher(key: &[u8; 16], id: u64) -> AesCtr {
    AesCtr::new(
        &hash16(format!("AirPlayStreamKey{id}").as_bytes(), key).into(),
        &hash16(format!("AirPlayStreamIV{id}").as_bytes(), key).into(),
    )
}
struct Pending {
    peer: VerifyingKey,
    peer_curve: [u8; 32],
    our_curve: [u8; 32],
    shared: [u8; 32],
    cipher: AesCtr,
}
impl Drop for Pending {
    fn drop(&mut self) {
        self.shared.zeroize();
    }
}
#[derive(Default)]
pub struct Pairing {
    pending: Option<Pending>,
    shared: Option<[u8; 32]>,
    setup: bool,
}
impl Drop for Pairing {
    fn drop(&mut self) {
        if let Some(s) = &mut self.shared {
            s.zeroize();
        }
    }
}
impl Pairing {
    pub fn setup(&mut self, body: &[u8], identity: &SigningKey) -> Result<Vec<u8>> {
        ensure!(
            !self.setup && self.pending.is_none() && self.shared.is_none() && body.len() == 32,
            "pair-setup sequence/length"
        );
        self.setup = true;
        Ok(identity.verifying_key().as_bytes().to_vec())
    }
    pub fn verify(&mut self, body: &[u8], identity: &SigningKey) -> Result<Vec<u8>> {
        let result = self.verify_inner(body, identity);
        if result.is_err() {
            self.pending = None;
            if let Some(mut s) = self.shared.take() {
                s.zeroize();
            }
            self.setup = false;
        }
        result
    }
    fn verify_inner(&mut self, body: &[u8], identity: &SigningKey) -> Result<Vec<u8>> {
        ensure!(
            body.len() >= 4 && body[1..4] == [0, 0, 0],
            "pair-verify header"
        );
        match body[0] {
            1 => {
                ensure!(
                    body.len() == 68 && self.pending.is_none() && self.shared.is_none(),
                    "pair-verify first stage"
                );
                let peer_curve: [u8; 32] = body[4..36].try_into()?;
                let peer = VerifyingKey::from_bytes(body[36..68].try_into()?)?;
                let mut random = [0; 32];
                OsRng.fill_bytes(&mut random);
                let secret = StaticSecret::from(random);
                random.zeroize();
                let our_curve = PublicKey::from(&secret).to_bytes();
                let shared = secret.diffie_hellman(&PublicKey::from(peer_curve));
                ensure!(shared.was_contributory(), "noncontributory peer key");
                let shared = shared.to_bytes();
                let mut message = our_curve.to_vec();
                message.extend_from_slice(&peer_curve);
                let mut signature = identity.sign(&message).to_bytes();
                let mut cipher = AesCtr::new(
                    &hash16(b"Pair-Verify-AES-Key", &shared).into(),
                    &hash16(b"Pair-Verify-AES-IV", &shared).into(),
                );
                cipher.apply_keystream(&mut signature);
                let mut response = our_curve.to_vec();
                response.extend_from_slice(&signature);
                self.pending = Some(Pending {
                    peer,
                    peer_curve,
                    our_curve,
                    shared,
                    cipher,
                });
                Ok(response)
            }
            0 => {
                ensure!(body.len() == 68, "pair-verify proof length");
                let mut p = self
                    .pending
                    .take()
                    .ok_or_else(|| anyhow::anyhow!("pair proof without exchange"))?;
                let mut sig: [u8; 64] = body[4..].try_into()?;
                p.cipher.apply_keystream(&mut sig);
                let mut msg = p.peer_curve.to_vec();
                msg.extend_from_slice(&p.our_curve);
                p.peer.verify_strict(&msg, &Signature::from_bytes(&sig))?;
                self.shared = Some(p.shared);
                Ok(Vec::new())
            }
            _ => bail!("unknown pairing step"),
        }
    }
    pub fn shared(&self) -> Result<&[u8; 32]> {
        self.shared
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("pair proof required"))
    }
}
#[derive(Default)]
pub struct FairPlay {
    stage: u8,
    message: Option<[u8; 164]>,
}
impl FairPlay {
    pub fn exchange(&mut self, body: &[u8]) -> Result<Vec<u8>> {
        let result = (|| {
            ensure!(
                body.len() >= 16 && &body[..4] == b"FPLY" && body[4..6] == [3, 1],
                "FairPlay header"
            );
            match body[6] {
                1 => {
                    ensure!(
                        self.stage == 0
                            && body.len() == 16
                            && body[14] < 4
                            && body[8..12] == [0, 0, 0, 4],
                        "FairPlay setup mode/length/state"
                    );
                    self.stage = 1;
                    let replies = include_bytes!("../../vendor/playfair/replies.bin");
                    let start = body[14] as usize * 142;
                    Ok(replies[start..start + 142].to_vec())
                }
                3 => {
                    ensure!(
                        self.stage == 1
                            && body.len() == 164
                            && body[12] < 4
                            && body[8..12] == [0, 0, 0, 152],
                        "FairPlay exchange mode/length/state"
                    );
                    self.message = Some(body.try_into()?);
                    self.stage = 2;
                    let mut out = b"FPLY\x03\x01\x04\x00\x00\x00\x00\x14".to_vec();
                    out.extend_from_slice(&body[144..]);
                    Ok(out)
                }
                _ => bail!("FairPlay sequence"),
            }
        })();
        if result.is_err() {
            self.stage = 0;
            self.message = None;
        }
        result
    }
    pub fn message(&self) -> Result<&[u8; 164]> {
        self.message
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("FairPlay exchange required"))
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_skipped_repeated_and_failed_proofs() {
        let id = SigningKey::from_bytes(&[1; 32]);
        let mut p = Pairing::default();
        assert!(p.verify(&[0; 68], &id).is_err());
        let secret = StaticSecret::from([2; 32]);
        let mut req = vec![1, 0, 0, 0];
        req.extend_from_slice(PublicKey::from(&secret).as_bytes());
        req.extend_from_slice(id.verifying_key().as_bytes());
        p.verify(&req, &id).unwrap();
        assert!(p.shared().is_err());
        assert!(p.verify(&[0; 68], &id).is_err());
        assert!(p.shared().is_err());
        p.verify(&req, &id).unwrap();
        assert!(p.verify(&req, &id).is_err());
        assert!(p.shared().is_err());
    }
    #[test]
    fn valid_proof_and_no_replay() {
        let id = SigningKey::from_bytes(&[1; 32]);
        let sender = SigningKey::from_bytes(&[5; 32]);
        let sec = StaticSecret::from([2; 32]);
        let pubkey = PublicKey::from(&sec);
        let mut p = Pairing::default();
        let mut req = vec![1, 0, 0, 0];
        req.extend_from_slice(pubkey.as_bytes());
        req.extend_from_slice(sender.verifying_key().as_bytes());
        let reply = p.verify(&req, &id).unwrap();
        let shared = sec.diffie_hellman(&PublicKey::from(
            <[u8; 32]>::try_from(&reply[..32]).unwrap(),
        ));
        let mut cipher = AesCtr::new(
            &hash16(b"Pair-Verify-AES-Key", shared.as_bytes()).into(),
            &hash16(b"Pair-Verify-AES-IV", shared.as_bytes()).into(),
        );
        cipher.apply_keystream(&mut [0; 64]);
        let mut signed = pubkey.as_bytes().to_vec();
        signed.extend_from_slice(&reply[..32]);
        let mut sig = sender.sign(&signed).to_bytes();
        cipher.apply_keystream(&mut sig);
        let mut proof = vec![0; 4];
        proof.extend_from_slice(&sig);
        p.verify(&proof, &id).unwrap();
        assert_eq!(p.shared().unwrap(), shared.as_bytes());
        assert!(p.verify(&proof, &id).is_err());
    }
    #[test]
    fn rejects_all_fairplay_modes_outside_table() {
        for mode in 4..=255 {
            let mut f = FairPlay::default();
            let mut req = *b"FPLY\x03\x01\x01\x00\x00\x00\x00\x04\x02\x00\x00\x00";
            req[14] = mode;
            assert!(f.exchange(&req).is_err());
        }
    }
    #[test]
    fn ctr_fragmentation_and_tiny_packets() {
        let key = [8; 16];
        let mut a = video_cipher(&key, 5);
        let mut b = video_cipher(&key, 5);
        let mut whole = [0; 99];
        a.apply_keystream(&mut whole);
        let mut pieces = [0; 99];
        for c in pieces.chunks_mut(3) {
            b.apply_keystream(c);
        }
        assert_eq!(whole, pieces);
    }
}
