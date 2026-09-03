export type SupportContent = {
  type: "text" | "photo" | "document";
  body: string | null;
  fileId: string | null;
  fileName: string | null;
  mimeType: string | null;
};

type MediaMessage = {
  text?: string;
  caption?: string;
  photo?: Array<{ file_id: string; file_size?: number }>;
  document?: { file_id: string; file_name?: string; mime_type?: string };
};

export function parseAdminIds(
  value: string | undefined,
  fallback: string,
): number[] {
  const source = (value?.trim() || fallback).split(",");
  const ids = source.map((part) => Number(part.trim())).filter((id) =>
    Number.isSafeInteger(id) && id > 0
  );
  return [...new Set(ids)];
}

export function supportContent(message: MediaMessage): SupportContent | null {
  const body = (message.text ?? message.caption ?? "").trim() || null;
  if (message.document?.file_id) {
    return {
      type: "document",
      body,
      fileId: message.document.file_id,
      fileName: message.document.file_name?.slice(0, 255) || "attachment",
      mimeType: message.document.mime_type?.slice(0, 127) || null,
    };
  }
  if (message.photo?.length) {
    const photo = [...message.photo].sort((a, b) =>
      (a.file_size ?? 0) - (b.file_size ?? 0)
    ).at(-1)!;
    return {
      type: "photo",
      body,
      fileId: photo.file_id,
      fileName: "screenshot.jpg",
      mimeType: "image/jpeg",
    };
  }
  return body
    ? { type: "text", body, fileId: null, fileName: null, mimeType: null }
    : null;
}

export function ticketIdFromCallback(
  value: string | undefined,
  action: "open" | "close",
): string | null {
  const match = value?.match(
    new RegExp(
      `^ticket:${action}:([0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})$`,
      "i",
    ),
  );
  return match?.[1]?.toLowerCase() ?? null;
}
