import {
  decryptLicense,
  digestLicense,
  encryptLicense,
  generateLicenseKey,
} from "./license.ts";

function assert(condition: boolean, message: string): void {
  if (!condition) throw new Error(message);
}

Deno.test("generates launcher-compatible unique license keys", () => {
  const keys = new Set(Array.from({ length: 1000 }, generateLicenseKey));
  assert(keys.size === 1000, "generated duplicate keys");
  for (const key of keys) {
    assert(
      /^AXM-(?:[A-HJ-NP-Z2-9]{5}-){3}[A-HJ-NP-Z2-9]{5}$/.test(key),
      `bad key: ${key}`,
    );
  }
});

Deno.test("HMAC digest is deterministic and normalized", async () => {
  const pepper = "test-pepper-with-at-least-32-characters";
  const upper = await digestLicense("AXM-ABCDE-FGHIJ-KLMNO-PQRST", pepper);
  const lower = await digestLicense(" axm-abcde-fghij-klmno-pqrst ", pepper);
  assert(upper === lower, "normalization changed digest");
  assert(/^[0-9a-f]{64}$/.test(upper), "digest is not SHA-256 hex");
});

Deno.test("AES-GCM license envelope round trips and detects tampering", async () => {
  const encryptionKey = btoa(
    String.fromCharCode(...crypto.getRandomValues(new Uint8Array(32))),
  );
  const license = generateLicenseKey();
  const encrypted = await encryptLicense(license, encryptionKey);
  assert(encrypted.startsWith("v1."), "missing envelope version");
  assert(
    await decryptLicense(encrypted, encryptionKey) === license,
    "round trip failed",
  );

  const last = encrypted.at(-1)!;
  const tampered = encrypted.slice(0, -1) + (last === "A" ? "B" : "A");
  let rejected = false;
  try {
    await decryptLicense(tampered, encryptionKey);
  } catch {
    rejected = true;
  }
  assert(rejected, "tampered ciphertext was accepted");
});
