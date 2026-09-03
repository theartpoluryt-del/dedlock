import { createClient } from "npm:@supabase/supabase-js@2";

const encoder = new TextEncoder();
const jsonHeaders = {
  "content-type": "application/json; charset=utf-8",
  "cache-control": "no-store",
  "x-content-type-options": "nosniff",
};
const watermarkPlaceholders = [1, 2, 3].map((slot) =>
  encoder.encode(`AXIOM-WATERMARK-SLOT-0${slot}`)
);
const releaseCache = new Map<
  string,
  { bytes: Uint8Array; offsets: number[] }
>();

function reply(status: number, body: Record<string, unknown>): Response {
  return new Response(JSON.stringify(body), { status, headers: jsonHeaders });
}

function requiredSecret(name: string): string {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`Missing secret: ${name}`);
  return value;
}

function bytesToHex(bytes: Uint8Array): string {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function hexToBytes(value: string): Uint8Array {
  if (value.length % 2 !== 0 || !/^[0-9a-f]+$/i.test(value)) {
    throw new Error("Invalid hexadecimal value");
  }
  const result = new Uint8Array(value.length / 2);
  for (let i = 0; i < result.length; i++) {
    result[i] = Number.parseInt(value.slice(i * 2, i * 2 + 2), 16);
  }
  return result;
}

function bytesToBase64Url(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(
    /=+$/,
    "",
  );
}

function base64ToBytes(value: string): Uint8Array {
  const binary = atob(value.replaceAll("\n", "").replaceAll("\r", ""));
  return Uint8Array.from(binary, (character) => character.charCodeAt(0));
}

async function hmacBytes(secret: string, value: string): Promise<Uint8Array> {
  const key = await crypto.subtle.importKey(
    "raw",
    encoder.encode(secret),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  return new Uint8Array(
    await crypto.subtle.sign("HMAC", key, encoder.encode(value)),
  );
}

async function hmacHex(secret: string, value: string): Promise<string> {
  return bytesToHex(await hmacBytes(secret, value));
}

async function sha256Hex(bytes: Uint8Array): Promise<string> {
  return bytesToHex(
    new Uint8Array(await crypto.subtle.digest("SHA-256", asArrayBuffer(bytes))),
  );
}

function derIntegerTo32(bytes: Uint8Array): Uint8Array {
  let offset = 0;
  while (offset < bytes.length - 1 && bytes[offset] === 0) offset++;
  const value = bytes.slice(offset);
  if (value.length > 32) throw new Error("Invalid ECDSA integer");
  const result = new Uint8Array(32);
  result.set(value, 32 - value.length);
  return result;
}

function normalizeP256Signature(signature: Uint8Array): Uint8Array {
  if (signature.length === 64) return signature;
  if (signature.length < 8 || signature[0] !== 0x30) {
    throw new Error("Invalid ECDSA signature");
  }
  let cursor = 2;
  if (signature[1] & 0x80) cursor = 2 + (signature[1] & 0x7f);
  if (signature[cursor++] !== 0x02) throw new Error("Invalid ECDSA signature");
  const rLength = signature[cursor++];
  const r = derIntegerTo32(signature.slice(cursor, cursor + rLength));
  cursor += rLength;
  if (signature[cursor++] !== 0x02) throw new Error("Invalid ECDSA signature");
  const sLength = signature[cursor++];
  const s = derIntegerTo32(signature.slice(cursor, cursor + sLength));
  const result = new Uint8Array(64);
  result.set(r, 0);
  result.set(s, 32);
  return result;
}

async function signingKey(): Promise<CryptoKey> {
  const encoded = base64ToBytes(
    requiredSecret("AXIOM_SIGNING_PRIVATE_KEY_B64"),
  );
  const keyData = encoded.buffer.slice(
    encoded.byteOffset,
    encoded.byteOffset + encoded.byteLength,
  ) as ArrayBuffer;
  return await crypto.subtle.importKey(
    "pkcs8",
    keyData,
    { name: "ECDSA", namedCurve: "P-256" },
    false,
    ["sign"],
  );
}

async function signToken(payload: string): Promise<string> {
  const signature = new Uint8Array(
    await crypto.subtle.sign(
      { name: "ECDSA", hash: "SHA-256" },
      await signingKey(),
      encoder.encode(payload),
    ),
  );
  return `${bytesToBase64Url(encoder.encode(payload))}.${
    bytesToBase64Url(normalizeP256Signature(signature))
  }`;
}

async function verificationKey(): Promise<CryptoKey> {
  return await crypto.subtle.importKey(
    "jwk",
    {
      kty: "EC",
      crv: "P-256",
      x: "wnfsMKWcXod9gpwseW3-micd2z4tL1QzwZlMNQTgNEQ",
      y: "TcpxPyVhvRyA-Da7-Qj71NMS2kvN4mqyU8JCufQkTx0",
      ext: true,
    },
    { name: "ECDSA", namedCurve: "P-256" },
    false,
    ["verify"],
  );
}

function base64UrlToBytes(value: string): Uint8Array {
  const padded = value.replaceAll("-", "+").replaceAll("_", "/") +
    "=".repeat((4 - value.length % 4) % 4);
  return base64ToBytes(padded);
}

function asArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  return bytes.buffer.slice(
    bytes.byteOffset,
    bytes.byteOffset + bytes.byteLength,
  ) as ArrayBuffer;
}

async function verifySessionToken(
  token: string,
): Promise<Record<string, string> | null> {
  try {
    const parts = token.split(".");
    if (parts.length !== 2) return null;
    const payload = base64UrlToBytes(parts[0]);
    const signature = base64UrlToBytes(parts[1]);
    if (
      signature.length !== 64 || !await crypto.subtle.verify(
        { name: "ECDSA", hash: "SHA-256" },
        await verificationKey(),
        asArrayBuffer(signature),
        asArrayBuffer(payload),
      )
    ) return null;
    const text = new TextDecoder("ascii", { fatal: true }).decode(payload);
    const params = new URLSearchParams(text);
    const expected = [
      "v",
      "license",
      "device",
      "expires",
      "nonce",
      "base",
      "sha256",
      "size",
      "slots",
    ];
    if (
      [...params.keys()].length !== expected.length ||
      expected.some((name) => !params.has(name))
    ) return null;
    const result = Object.fromEntries(
      expected.map((name) => [name, params.get(name)!]),
    );
    if (
      result.v !== "1" ||
      Number(result.expires) <= Math.floor(Date.now() / 1000) ||
      !/^[0-9a-f]{64}$/.test(result.device) ||
      !/^[0-9a-f]{64}$/.test(result.base) ||
      !/^[0-9a-f]{64}$/.test(result.sha256) ||
      !/^\d+$/.test(result.size) ||
      !parseOffsets(result.slots, Number(result.size))
    ) return null;
    return result;
  } catch {
    return null;
  }
}

function storageObjectUrl(base: string, bucket: string, path: string): string {
  const encoded = path.split("/").map(encodeURIComponent).join("/");
  return `${base.replace(/\/$/, "")}/storage/v1/object/authenticated/${
    encodeURIComponent(bucket)
  }/${encoded}`;
}

function findUniquePlaceholder(bytes: Uint8Array, needle: Uint8Array): number {
  let found = -1;
  outer:
  for (let offset = 0; offset + 64 <= bytes.length; offset++) {
    for (let i = 0; i < needle.length; i++) {
      if (bytes[offset + i] !== needle[i]) continue outer;
    }
    if (found !== -1) throw new Error("Duplicate watermark placeholder");
    found = offset;
  }
  if (found < 0) throw new Error("Missing watermark placeholder");
  return found;
}

async function loadRelease(
  supabaseUrl: string,
  serviceKey: string,
  release: Record<string, unknown>,
): Promise<{ bytes: Uint8Array; offsets: number[] }> {
  const baseHash = String(release.sha256 ?? "");
  const cached = releaseCache.get(baseHash);
  if (cached) return cached;
  const storage = await fetch(
    storageObjectUrl(
      supabaseUrl,
      String(release.storage_bucket),
      String(release.storage_path),
    ),
    { headers: { authorization: `Bearer ${serviceKey}`, apikey: serviceKey } },
  );
  if (!storage.ok) throw new Error("Unable to read release module");
  const bytes = new Uint8Array(await storage.arrayBuffer());
  if (
    bytes.length !== Number(release.byte_size) ||
    await sha256Hex(bytes) !== baseHash
  ) throw new Error("Release module integrity failure");
  const offsets = watermarkPlaceholders.map((marker) =>
    findUniquePlaceholder(bytes, marker)
  );
  const result = { bytes, offsets };
  releaseCache.clear();
  releaseCache.set(baseHash, result);
  return result;
}

async function watermarkSlots(
  license: string,
  device: string,
  releaseHash: string,
): Promise<Uint8Array[]> {
  const licenseBytes = hexToBytes(license.replaceAll("-", ""));
  const deviceBytes = hexToBytes(device);
  const releaseBytes = hexToBytes(releaseHash);
  if (licenseBytes.length !== 16 || deviceBytes.length !== 32) {
    throw new Error("Invalid watermark identity");
  }
  return await Promise.all([1, 2, 3].map(async (slotNumber) => {
    const slot = new Uint8Array(64);
    slot.set(encoder.encode("AXWM"), 0);
    slot[4] = 1;
    slot[5] = slotNumber;
    slot.set(licenseBytes, 8);
    slot.set(deviceBytes.slice(0, 16), 24);
    slot.set(releaseBytes.slice(0, 8), 40);
    const tag = await hmacBytes(
      requiredSecret("LICENSE_PEPPER"),
      `watermark:v1:${license}:${device}:${releaseHash}:${slotNumber}`,
    );
    slot.set(tag.slice(0, 16), 48);
    return slot;
  }));
}

function parseOffsets(value: string, declaredSize: number): number[] | null {
  if (!/^\d+(,\d+){2}$/.test(value)) return null;
  const offsets = value.split(",").map(Number);
  if (
    offsets.some((offset) =>
      !Number.isSafeInteger(offset) || offset < 0 || offset + 64 > declaredSize
    ) || new Set(offsets).size !== offsets.length
  ) return null;
  return offsets;
}

async function serveModuleChunk(request: Request): Promise<Response> {
  const authorization = request.headers.get("authorization") ?? "";
  if (!authorization.startsWith("Bearer ")) return reply(401, { ok: false });
  const fields = await verifySessionToken(authorization.slice(7).trim());
  if (!fields) return reply(401, { ok: false });
  const url = new URL(request.url);
  const offset = Number(url.searchParams.get("offset"));
  const length = Number(url.searchParams.get("length"));
  const declaredSize = Number(fields.size);
  const offsets = parseOffsets(fields.slots, declaredSize);
  if (
    !Number.isSafeInteger(offset) || !Number.isSafeInteger(length) ||
    offset < 0 || length < 1 || length > 512 * 1024 ||
    offset >= declaredSize || offset + length > declaredSize || !offsets
  ) {
    return reply(400, { ok: false });
  }
  const supabaseUrl = requiredSecret("SUPABASE_URL");
  const serviceKey = requiredSecret("SUPABASE_SERVICE_ROLE_KEY");
  const supabase = createClient(supabaseUrl, serviceKey, {
    auth: { persistSession: false, autoRefreshToken: false },
  });
  const { data: release, error } = await supabase.rpc(
    "axiom_authorize_download",
    {
      p_license_id: fields.license,
      p_device_hash: fields.device,
    },
  );
  if (
    error || !release || release.sha256 !== fields.base ||
    Number(release.byte_size) !== declaredSize
  ) return reply(403, { ok: false });
  const storage = await fetch(
    storageObjectUrl(supabaseUrl, release.storage_bucket, release.storage_path),
    {
      headers: {
        authorization: `Bearer ${serviceKey}`,
        apikey: serviceKey,
        range: `bytes=${offset}-${offset + length - 1}`,
      },
    },
  );
  if (!storage.ok) return reply(503, { ok: false });
  let bytes = new Uint8Array(await storage.arrayBuffer());
  if (bytes.length === declaredSize) {
    bytes = bytes.slice(offset, offset + length);
  }
  if (bytes.length !== length) return reply(503, { ok: false });
  const personalized = await watermarkSlots(
    fields.license,
    fields.device,
    fields.base,
  );
  for (let slotIndex = 0; slotIndex < offsets.length; slotIndex++) {
    const slotStart = offsets[slotIndex];
    const overlapStart = Math.max(offset, slotStart);
    const overlapEnd = Math.min(offset + length, slotStart + 64);
    if (overlapStart >= overlapEnd) continue;
    bytes.set(
      personalized[slotIndex].slice(
        overlapStart - slotStart,
        overlapEnd - slotStart,
      ),
      overlapStart - offset,
    );
  }
  return new Response(bytes, {
    status: 200,
    headers: {
      "content-type": "application/octet-stream",
      "content-length": String(bytes.length),
      "cache-control": "no-store",
      "x-content-type-options": "nosniff",
    },
  });
}

function versionParts(value: string): number[] | null {
  if (!/^\d{1,5}\.\d{1,5}\.\d{1,5}$/.test(value)) return null;
  return value.split(".").map(Number);
}

function versionAtLeast(actual: string, required: string): boolean {
  const a = versionParts(actual);
  const b = versionParts(required);
  if (!a || !b) return false;
  for (let i = 0; i < 3; i++) {
    if (a[i] !== b[i]) return a[i] > b[i];
  }
  return true;
}

Deno.serve(async (request: Request) => {
  if (request.method === "GET") return await serveModuleChunk(request);
  if (request.method !== "POST") return reply(405, { ok: false });
  const contentLength = Number(request.headers.get("content-length") ?? "0");
  if (contentLength > 8192) return reply(413, { ok: false });

  try {
    const requestText = await request.text();
    if (encoder.encode(requestText).length > 8192) {
      return reply(413, { ok: false });
    }
    const body = JSON.parse(requestText);
    const key = String(body?.key ?? "").trim().toUpperCase();
    const device = String(body?.device ?? "");
    const nonce = String(body?.nonce ?? "");
    const launcherVersion = String(body?.version ?? "");
    if (
      key.length < 12 || key.length > 96 || !/^[A-Z0-9-]+$/.test(key) ||
      !/^[0-9a-f]{64}$/.test(device) || !/^[0-9a-f]{32}$/.test(nonce) ||
      !versionParts(launcherVersion)
    ) {
      return reply(400, { ok: false });
    }

    const forwarded = request.headers.get("cf-connecting-ip") ??
      request.headers.get("x-real-ip") ??
      request.headers.get("x-forwarded-for")?.split(",")[0]?.trim() ??
      "unknown";
    const pepper = requiredSecret("LICENSE_PEPPER");
    const keyHash = await hmacHex(pepper, key);
    const nonceHash = await hmacHex(pepper, nonce);
    const ipHash = await hmacHex(requiredSecret("IP_HASH_PEPPER"), forwarded);
    const supabaseUrl = requiredSecret("SUPABASE_URL");
    const serviceKey = requiredSecret("SUPABASE_SERVICE_ROLE_KEY");
    const supabase = createClient(
      supabaseUrl,
      serviceKey,
      { auth: { persistSession: false, autoRefreshToken: false } },
    );

    const { data: verification, error: verificationError } = await supabase.rpc(
      "axiom_verify_and_bind_license",
      {
        p_key_hash: keyHash,
        p_device_hash: device,
        p_nonce_hash: nonceHash,
        p_ip_hash: ipHash,
      },
    );
    if (verificationError) throw verificationError;
    if (!verification?.ok) {
      if (verification?.reason === "rate_limited") {
        return reply(429, { ok: false });
      }
      if (
        verification?.reason === "device_limit" ||
        verification?.reason === "device_revoked" ||
        verification?.reason === "trial_device_used" ||
        verification?.reason === "trial_device_locked" ||
        verification?.reason === "trial_network_limit"
      ) {
        return reply(403, { ok: false });
      }
      return reply(401, { ok: false });
    }

    const release = verification.release;
    if (!release) return reply(503, { ok: false });
    if (!versionAtLeast(launcherVersion, release.minimum_launcher_version)) {
      return reply(426, { ok: false, update_required: true });
    }

    const baseRelease = await loadRelease(supabaseUrl, serviceKey, release);
    const personalized = baseRelease.bytes.slice();
    const slots = await watermarkSlots(
      verification.license_id,
      device,
      release.sha256,
    );
    for (let i = 0; i < baseRelease.offsets.length; i++) {
      personalized.set(slots[i], baseRelease.offsets[i]);
    }
    const personalizedHash = await sha256Hex(personalized);

    const expires = Math.floor(Date.now() / 1000) + 300;
    const payload = [
      "v=1",
      `license=${verification.license_id}`,
      `device=${device}`,
      `expires=${expires}`,
      `nonce=${nonce}`,
      `base=${release.sha256}`,
      `sha256=${personalizedHash}`,
      `size=${release.byte_size}`,
      `slots=${baseRelease.offsets.join(",")}`,
    ].join("&");
    return reply(200, {
      ok: true,
      token: await signToken(payload),
      expires,
      release: release.version,
    });
  } catch (error) {
    console.error(
      error instanceof Error ? error.message : "license request failed",
    );
    return reply(503, { ok: false });
  }
});
