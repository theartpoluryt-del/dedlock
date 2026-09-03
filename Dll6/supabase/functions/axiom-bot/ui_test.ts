import { assertEquals } from "jsr:@std/assert@1";
import {
  commandDefinitions,
  commandFromText,
  navigationKeyboard,
  normalizeLocale,
} from "./ui.ts";

Deno.test("normalizes supported locales", () => {
  assertEquals(normalizeLocale("en"), "en");
  assertEquals(normalizeLocale("de"), "ru");
  assertEquals(normalizeLocale(null), "ru");
});

Deno.test("parses commands addressed to the bot", () => {
  assertEquals(commandFromText("/download@AxiomLauncherBot now"), "download");
  assertEquals(commandFromText("hello"), null);
});

Deno.test("publishes all requested commands in both languages", () => {
  const expected = [
    "download",
    "buy",
    "activate",
    "language",
    "keys",
    "trial",
    "guide",
    "support",
  ];
  assertEquals(commandDefinitions("ru").map((item) => item.command), expected);
  assertEquals(commandDefinitions("en").map((item) => item.command), expected);
});

Deno.test("context keyboards always include a route back to the menu", () => {
  for (
    const section of [
      "download",
      "buy",
      "activate",
      "language",
      "keys",
      "trial",
      "guide",
      "support",
    ] as const
  ) {
    const callbacks = navigationKeyboard("ru", section).flat().map((button) =>
      button.callback_data
    );
    assertEquals(callbacks.includes("nav:menu"), true);
  }
});

Deno.test("trial navigation opens the warning, not activation", () => {
  const callbacks = navigationKeyboard("ru", "menu").flat().map((button) =>
    button.callback_data
  );
  assertEquals(callbacks.includes("nav:trial"), true);
  assertEquals(callbacks.includes("trial:confirm"), false);
});
