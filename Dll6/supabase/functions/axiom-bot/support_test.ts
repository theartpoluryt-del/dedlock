import {
  parseAdminIds,
  supportContent,
  ticketIdFromCallback,
} from "./support.ts";

function assert(condition: boolean, message: string): void {
  if (!condition) throw new Error(message);
}

Deno.test("support admin allowlist is strict and deduplicated", () => {
  const ids = parseAdminIds(" 12,broken,12,34,-1 ", "99");
  assert(JSON.stringify(ids) === "[12,34]", "unexpected allowlist");
  assert(parseAdminIds(undefined, "99")[0] === 99, "fallback ignored");
});

Deno.test("support content accepts text, photos, and documents", () => {
  assert(supportContent({ text: " hello " })?.body === "hello", "text failed");
  assert(
    supportContent({
      photo: [{ file_id: "small", file_size: 1 }, {
        file_id: "large",
        file_size: 2,
      }],
    })
      ?.fileId === "large",
    "largest photo was not selected",
  );
  assert(
    supportContent({ document: { file_id: "doc", file_name: "log.txt" } })
      ?.fileName === "log.txt",
    "document failed",
  );
  assert(supportContent({}) === null, "empty message accepted");
});

Deno.test("ticket callbacks require a UUID and exact action", () => {
  const id = "b3ea133e-8db7-4d56-a577-3e772f855af4";
  assert(
    ticketIdFromCallback(`ticket:open:${id}`, "open") === id,
    "valid callback rejected",
  );
  assert(
    ticketIdFromCallback(`ticket:close:${id}`, "open") === null,
    "wrong action accepted",
  );
  assert(
    ticketIdFromCallback("ticket:open:../bad", "open") === null,
    "bad id accepted",
  );
});
