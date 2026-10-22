//! Narrow HAP transient SRP-6a adapter; no SRP arithmetic is implemented here.
//!
//! Math/proof verification: RustCrypto `srp` 0.7.0-rc.3 (MIT OR Apache-2.0),
//! source d40665b4ead69ad5e7933413a5158672366b8b9f, SHA-256
//! 4b5f622c2d21b826b3501f4c620ccc4696e4a09a6b51727fcb21036dec87c101.
//! HAP encoding: Apple HomeKitADK fb201f98f5fdc7fef6a455054f08b59cca5d1ec8,
//! PAL/Crypto/OpenSSL/HAPOpenSSL.c and Tests/HAPCryptoTest.c. Facts only; no
//! Apple implementation code is imported. The crate's HomeKit `g_no_pad`
//! comment is reversed relative to those test vectors: HAP needs `true`.
//!
//! Owned private bytes are zeroized. Upstream's temporary big integers and
//! private ServerVerifier vectors do not expose zeroization and are only
//! dropped promptly; this adapter does not claim to erase those allocations.
//! The upstream crate is a prerelease and explicitly has no independent audit.

use anyhow::{Context, Result, ensure};
use rand::{RngCore, rngs::OsRng};
use sha2_11::Sha512;
use srp::{ClientG3072, ServerG3072, groups::Group};
use zeroize::Zeroizing;

const USERNAME: &[u8] = b"Pair-Setup";
const TRANSIENT_PASSWORD: &[u8] = b"3939";
const PUBLIC_BYTES: usize = 384;
const PROOF_BYTES: usize = 64;

/// One fresh transient setup. Consuming `verify` prevents retrying a failed
/// proof with the same ephemeral secret. The transport must also fence M1/M3.
/// Intentionally neither Clone nor Debug: do not duplicate or log secrets.
pub struct SrpSetup {
    username: &'static [u8],
    salt: [u8; 16],
    public: [u8; PUBLIC_BYTES],
    private: Zeroizing<[u8; 32]>,
    verifier: Zeroizing<Vec<u8>>,
}

fn trim_public(bytes: &[u8]) -> &[u8] {
    &bytes[bytes
        .iter()
        .position(|&byte| byte != 0)
        .unwrap_or(bytes.len())..]
}

fn normalize_public(bytes: &[u8]) -> Result<[u8; PUBLIC_BYTES]> {
    ensure!(
        (1..=PUBLIC_BYTES).contains(&bytes.len()),
        "HAP SRP public-key length"
    );
    ensure!(!trim_public(bytes).is_empty(), "HAP SRP public key is zero");
    let mut padded = [0; PUBLIC_BYTES];
    padded[PUBLIC_BYTES - bytes.len()..].copy_from_slice(bytes);
    let group = srp::groups::G3072::generator();
    let modulus = group.params().modulus().to_be_bytes();
    ensure!(
        padded.as_slice() < modulus.as_ref(),
        "HAP SRP public key outside the fixed group"
    );
    Ok(padded)
}

impl SrpSetup {
    pub fn new() -> Result<Self> {
        let mut salt = [0; 16];
        let mut private = Zeroizing::new([0; 32]);
        OsRng
            .try_fill_bytes(&mut salt)
            .context("HAP SRP salt randomness unavailable")?;
        OsRng
            .try_fill_bytes(private.as_mut())
            .context("HAP SRP secret randomness unavailable")?;
        Self::from_parts(USERNAME, TRANSIENT_PASSWORD, salt, private)
    }

    fn from_parts(
        username: &'static [u8],
        password: &[u8],
        salt: [u8; 16],
        private: Zeroizing<[u8; 32]>,
    ) -> Result<Self> {
        ensure!(private.iter().any(|&byte| byte != 0), "HAP SRP zero secret");
        let verifier = Zeroizing::new(
            ClientG3072::<Sha512>::new().compute_verifier(username, password, &salt),
        );
        ensure!(
            (1..=PUBLIC_BYTES).contains(&verifier.len()),
            "HAP SRP verifier width"
        );
        let public = ServerG3072::<Sha512>::new_with_options(true)
            .compute_public_ephemeral(private.as_ref(), &verifier);
        Ok(Self {
            username,
            salt,
            public: normalize_public(&public)?,
            private,
            verifier,
        })
    }

    pub fn salt(&self) -> &[u8; 16] {
        &self.salt
    }

    pub fn public(&self) -> &[u8; PUBLIC_BYTES] {
        &self.public
    }

    /// Return (verified SHA-512 session key K, server proof M2). These owned
    /// return buffers belong to the caller, which must zeroize retained keys.
    pub fn verify(self, a: &[u8], m1: &[u8]) -> Result<([u8; 64], [u8; 64])> {
        ensure!(m1.len() == PROOF_BYTES, "HAP SRP client-proof length");
        let padded_a = normalize_public(a)?;
        // The crate pads A/B for u and trims S for K. Supplying trimmed A and
        // selecting unpadded g gives HAP's M1 without changing the SRP math.
        let pending = ServerG3072::<Sha512>::new_with_options(true)
            .process_reply(
                self.username,
                &self.salt,
                self.private.as_ref(),
                &self.verifier,
                trim_public(&padded_a),
            )
            .context("HAP SRP public-key verification failed")?;
        // verify_client uses subtle::ConstantTimeEq. Never release K or M2
        // before it succeeds; key() is the raw premaster and is not HAP's K.
        let key: [u8; 64] = pending
            .verify_client(m1)
            .context("HAP SRP client proof failed")?
            .try_into()
            .context("HAP SRP session-key width")?;
        let key = Zeroizing::new(key);
        drop(pending);
        // Apple M2 deliberately uses full-width A, unlike the trimmed A in M1.
        // Reuse upstream's proof primitive with that wire encoding, rather than
        // pending.proof(), which would reuse the M1 input's trimmed A.
        let proof_input: [u8; 64] = m1.try_into()?;
        let proof = srp::utils::compute_m2::<Sha512>(&padded_a, &proof_input.into(), key.as_ref());
        let proof: [u8; 64] = proof
            .as_slice()
            .try_into()
            .context("HAP SRP server-proof width")?;
        Ok((*key, proof))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    fn fixture() -> Value {
        serde_json::from_str(include_str!("../../tests/fixtures/hap-srp-vector.json")).unwrap()
    }

    fn bytes(vector: &Value, field: &str) -> Vec<u8> {
        hex::decode(vector[field].as_str().unwrap()).unwrap()
    }

    fn setup(vector: &Value) -> SrpSetup {
        assert_eq!(vector["username"], "alice");
        SrpSetup::from_parts(
            b"alice",
            vector["password"].as_str().unwrap().as_bytes(),
            bytes(vector, "salt_hex").try_into().unwrap(),
            Zeroizing::new(bytes(vector, "server_private_hex").try_into().unwrap()),
        )
        .unwrap()
    }

    fn check_vector(vector: &Value) {
        let server = setup(vector);
        assert_eq!(server.salt().as_slice(), bytes(vector, "salt_hex"));
        assert_eq!(
            server.public().as_slice(),
            bytes(vector, "server_public_hex")
        );
        assert_eq!(
            server.verifier.as_slice(),
            trim_public(&bytes(vector, "verifier_hex"))
        );
        let (key, proof) = server
            .verify(
                &bytes(vector, "client_public_hex"),
                &bytes(vector, "client_proof_hex"),
            )
            .unwrap();
        assert_eq!(key.as_slice(), bytes(vector, "session_key_hex"));
        assert_eq!(proof.as_slice(), bytes(vector, "server_proof_hex"));
    }

    #[test]
    fn matches_official_apple_hap_known_answers() {
        check_vector(&fixture()["vector"]);
    }

    #[test]
    fn malformed_public_values_and_proofs_fail_closed() {
        let fixture = fixture();
        let vector = &fixture["vector"];
        let modulus = bytes(&fixture["group"], "prime_hex");
        let a = bytes(vector, "client_public_hex");
        let proof = bytes(vector, "client_proof_hex");
        for invalid in [
            vec![],
            vec![0; PUBLIC_BYTES],
            vec![1; PUBLIC_BYTES + 1],
            modulus,
            vec![255; PUBLIC_BYTES],
        ] {
            assert!(setup(vector).verify(&invalid, &proof).is_err());
        }
        for size in [0, 63, 65] {
            assert!(setup(vector).verify(&a, &vec![0; size]).is_err());
        }
        let mut wrong = proof.clone();
        wrong[0] ^= 1;
        assert!(setup(vector).verify(&a, &wrong).is_err());
    }

    #[test]
    fn leading_zero_encodings_match_independent_derived_vectors() {
        let fixture = fixture();
        let vectors = fixture["derived_vectors"].as_array().unwrap();
        assert_eq!(vectors.len(), 3);
        let mut covered_a = false;
        let mut covered_b = false;
        let mut covered_s = false;
        for vector in vectors {
            check_vector(vector);
            let a = bytes(vector, "client_public_hex");
            let b = bytes(vector, "server_public_hex");
            let s = bytes(vector, "premaster_secret_hex");
            covered_a |= a[0] == 0;
            covered_b |= b[0] == 0;
            covered_s |= s[0] == 0;
            // Shortened wire A and its padded form must confirm the same key
            // and full-width-A proof, including A=5 (383 leading zero bytes).
            let (key, proof) = setup(vector)
                .verify(trim_public(&a), &bytes(vector, "client_proof_hex"))
                .unwrap();
            assert_eq!(key.as_slice(), bytes(vector, "session_key_hex"));
            assert_eq!(proof.as_slice(), bytes(vector, "server_proof_hex"));
        }
        assert!(covered_a && covered_b && covered_s);
    }

    #[test]
    fn verifier_leading_zero_padding_matches_apple_regression_input() {
        let fixture = fixture();
        let vector = &fixture["padding_regressions"][0];
        let salt = bytes(vector, "salt_hex");
        let verifier = ClientG3072::<Sha512>::new().compute_verifier(b"", b"", &salt);
        let expected = bytes(vector, "verifier_hex");
        assert_eq!(expected.len(), PUBLIC_BYTES);
        assert_eq!(expected[0], 0);
        assert_eq!(normalize_public(&verifier).unwrap().as_slice(), expected);
    }

    #[test]
    fn hap_requires_the_unpadded_generator_proof_option() {
        let fixture = fixture();
        let vector = &fixture["vector"];
        let server = setup(vector);
        let wrong = ServerG3072::<Sha512>::new_with_options(false)
            .process_reply(
                b"alice",
                server.salt(),
                server.private.as_ref(),
                &server.verifier,
                &bytes(vector, "client_public_hex"),
            )
            .unwrap();
        assert!(
            wrong
                .verify_client(&bytes(vector, "client_proof_hex"))
                .is_err()
        );
    }
}
