const encoder = new TextEncoder();
const decoder = new TextDecoder();
const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const activationPattern = /^[A-HJ-NP-Z2-9]{16}$/;
const legacyActivationPattern =
  /^AXF-(?:[A-HJ-NP-Z2-9]{5}-){3}[A-HJ-NP-Z2-9]{5}$/;

function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (const value of bytes) binary += String.fromCharCode(value);
  return btoa(binary);
}

function base64ToBytes(value: string): Uint8Array {
  const binary = atob(value);
  return Uint8Array.from(binary, (character) => character.charCodeAt(0));
}

export function generateLicenseKey(): string {
  const random = crypto.getRandomValues(new Uint8Array(20));
  const groups: string[] = [];
  for (let group = 0; group < 4; group++) {
    let part = "";
    for (let index = 0; index < 5; index++) {
      part += alphabet[random[group * 5 + index] % alphabet.length];
    }
    groups.push(part);
  }
  return `AXM-${groups.join("-")}`;
}

export function generateActivationCode(): string {
  const random = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(random, (value) => alphabet[value % alphabet.length]).join(
    "",
  );
}

export function normalizeActivationCode(value: string): string | null {
  const normalized = value.trim().toUpperCase();
  return activationPattern.test(normalized) ||
      legacyActivationPattern.test(normalized)
    ? normalized
    : null;
}

async function hmacDigest(value: string, pepper: string): Promise<string> {
  if (pepper.length < 32) {
    throw new Error("HMAC pepper must be at least 32 characters");
  }
  const cryptoKey = await crypto.subtle.importKey(
    "raw",
    encoder.encode(pepper),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const digest = new Uint8Array(
    await crypto.subtle.sign("HMAC", cryptoKey, encoder.encode(value)),
  );
  return Array.from(digest, (byte) => byte.toString(16).padStart(2, "0")).join(
    "",
  );
}

export async function digestActivationCode(
  code: string,
  pepper: string,
): Promise<string> {
  const normalized = normalizeActivationCode(code);
  if (!normalized) throw new Error("Invalid activation code");
  return await hmacDigest(normalized, pepper);
}

export async function digestLicense(
  key: string,
  pepper: string,
): Promise<string> {
  return await hmacDigest(key.trim().toUpperCase(), pepper);
}

function encryptionKey(base64Key: string): Promise<CryptoKey> {
  const raw = base64ToBytes(base64Key);
  if (raw.length !== 32) {
    throw new Error("BOT_KEY_ENCRYPTION_KEY must decode to 32 bytes");
  }
  return crypto.subtle.importKey(
    "raw",
    raw.slice().buffer as ArrayBuffer,
    "AES-GCM",
    false,
    ["encrypt", "decrypt"],
  );
}

export async function encryptLicense(
  key: string,
  base64Key: string,
): Promise<string> {
  const nonce = crypto.getRandomValues(new Uint8Array(12));
  const encrypted = new Uint8Array(
    await crypto.subtle.encrypt(
      { name: "AES-GCM", iv: nonce },
      await encryptionKey(base64Key),
      encoder.encode(key),
    ),
  );
  const result = new Uint8Array(nonce.length + encrypted.length);
  result.set(nonce);
  result.set(encrypted, nonce.length);
  return `v1.${bytesToBase64(result)}`;
}

export async function decryptLicense(
  ciphertext: string,
  base64Key: string,
): Promise<string> {
  const [version, payload] = ciphertext.split(".", 2);
  if (version !== "v1" || !payload) {
    throw new Error("Unsupported key ciphertext");
  }
  const packed = base64ToBytes(payload);
  if (packed.length < 29) throw new Error("Invalid key ciphertext");
  const clear = await crypto.subtle.decrypt(
    { name: "AES-GCM", iv: packed.slice(0, 12) },
    await encryptionKey(base64Key),
    packed.slice(12),
  );
  return decoder.decode(clear);
}
