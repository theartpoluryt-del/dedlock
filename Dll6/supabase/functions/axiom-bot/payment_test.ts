import { DisabledPaymentProvider, PaymentUnavailableError } from "./payment.ts";

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
