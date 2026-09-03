import {
  DisabledPaymentProvider,
  orderIdFromTelegramPayload,
  PaymentUnavailableError,
  rubMinorToStars,
  telegramInvoicePayload,
} from "./payment.ts";

function assert(condition: boolean, message: string): void {
  if (!condition) throw new Error(message);
}

Deno.test("disabled provider fails closed for checkout and webhooks", async () => {
  const provider = new DisabledPaymentProvider();
  let checkoutRejected = false;
  try {
    await provider.createCheckout({
      id: crypto.randomUUID(),
      title: "30 days",
      amountMinor: 99000,
      currency: "RUB",
      telegramUserId: 1,
    });
  } catch (error) {
    checkoutRejected = error instanceof PaymentUnavailableError;
  }

  let webhookRejected = false;
  try {
    await provider.verifyWebhook(new Request("https://example.invalid"));
  } catch (error) {
    webhookRejected = error instanceof PaymentUnavailableError;
  }

  assert(checkoutRejected, "disabled provider created a checkout");
  assert(webhookRejected, "disabled provider accepted a webhook");
});

Deno.test("RUB catalog prices are rounded up to whole Stars", () => {
  assert(rubMinorToStars(19000, 1.25) === 152, "3-day quote changed");
  assert(rubMinorToStars(29000, 1.25) === 232, "7-day quote changed");
  assert(rubMinorToStars(99000, 1.25) === 792, "30-day quote changed");
  assert(
    rubMinorToStars(101, 1.25) === 1,
    "fractional Star was not rounded up",
  );
});

Deno.test("Telegram invoice payload only accepts Axiom UUID orders", () => {
  const id = "f02d692a-9480-4bf1-8e23-fbddf5f0fca7";
  const payload = telegramInvoicePayload(id);
  assert(
    orderIdFromTelegramPayload(payload) === id,
    "payload did not round-trip",
  );
  assert(
    orderIdFromTelegramPayload(`other:${id}`) === null,
    "foreign payload accepted",
  );
  assert(
    orderIdFromTelegramPayload("axiom-order:not-a-uuid") === null,
    "bad UUID accepted",
  );
});
