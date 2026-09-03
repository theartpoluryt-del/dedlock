export type Checkout = {
  url: string;
  externalCheckoutId: string;
};

export type OrderForCheckout = {
  id: string;
  title: string;
  amountMinor: number;
  currency: string;
  telegramUserId: number;
};

export type VerifiedPayment = {
  provider: string;
  eventId: string;
  externalPaymentId: string;
  orderId: string;
  amountMinor: number;
  currency: string;
};

export interface PaymentProvider {
  readonly name: string;
  createCheckout(order: OrderForCheckout): Promise<Checkout>;
  verifyWebhook(request: Request): Promise<VerifiedPayment>;
}

export class PaymentUnavailableError extends Error {}

const invoicePrefix = "axiom-order:";

export function rubMinorToStars(
  rubAmountMinor: number,
  rubPerStar: number,
): number {
  if (!Number.isSafeInteger(rubAmountMinor) || rubAmountMinor <= 0) {
    throw new Error("Invalid RUB amount");
  }
  if (!Number.isFinite(rubPerStar) || rubPerStar <= 0) {
    throw new Error("Invalid Stars exchange rate");
  }
  return Math.ceil(rubAmountMinor / (rubPerStar * 100));
}

export function telegramInvoicePayload(orderId: string): string {
  if (
    !/^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i
      .test(orderId)
  ) {
    throw new Error("Invalid order id");
  }
  return `${invoicePrefix}${orderId.toLowerCase()}`;
}

export function orderIdFromTelegramPayload(payload: string): string | null {
  if (!payload.startsWith(invoicePrefix)) return null;
  const orderId = payload.slice(invoicePrefix.length).toLowerCase();
  try {
    telegramInvoicePayload(orderId);
    return orderId;
  } catch {
    return null;
  }
}

type Fetcher = typeof fetch;

/**
 * Telegram-native adapter for digital goods. It creates an XTR invoice link;
 * payment confirmation is accepted only from Telegram's authenticated update
 * webhook and is handled in index.ts.
 */
export class TelegramStarsPaymentProvider implements PaymentProvider {
  readonly name = "telegram_stars";

  constructor(
    private readonly botToken: string,
    private readonly fetcher: Fetcher = fetch,
  ) {
    if (!botToken.trim()) throw new Error("Missing Telegram bot token");
  }

  async createCheckout(order: OrderForCheckout): Promise<Checkout> {
    if (
      order.currency !== "XTR" || !Number.isSafeInteger(order.amountMinor) ||
      order.amountMinor <= 0
    ) {
      throw new Error("Telegram Stars checkout requires a positive XTR amount");
    }
    const response = await this.fetcher(
      `https://api.telegram.org/bot${this.botToken}/createInvoiceLink`,
      {
        method: "POST",
        headers: { "content-type": "application/json; charset=utf-8" },
        body: JSON.stringify({
          title: `Axiom: ${order.title}`.slice(0, 32),
          description: `AxiomLauncher license subscription: ${order.title}`,
          payload: telegramInvoicePayload(order.id),
          provider_token: "",
          currency: "XTR",
          prices: [{ label: order.title, amount: order.amountMinor }],
        }),
      },
    );
    const body = await response.json() as { ok?: boolean; result?: string };
    if (!response.ok || !body.ok || typeof body.result !== "string") {
      throw new Error("Telegram createInvoiceLink failed");
    }
    return { url: body.result, externalCheckoutId: order.id };
  }

  verifyWebhook(_request: Request): Promise<VerifiedPayment> {
    return Promise.reject(
      new PaymentUnavailableError(
        "Telegram Stars payments are verified through Telegram updates",
      ),
    );
  }
}

/**
 * Safe default used until a real merchant is selected.  It never returns a
 * checkout URL and never accepts a webhook as proof of payment.
 */
export class DisabledPaymentProvider implements PaymentProvider {
  readonly name = "disabled";

  createCheckout(_order: OrderForCheckout): Promise<Checkout> {
    return Promise.reject(
      new PaymentUnavailableError("Payment provider is not configured"),
    );
  }

  verifyWebhook(_request: Request): Promise<VerifiedPayment> {
    return Promise.reject(
      new PaymentUnavailableError("Payment webhooks are disabled"),
    );
  }
}

export function configuredPaymentProvider(): PaymentProvider {
  const selected = (Deno.env.get("PAYMENT_PROVIDER") ?? "disabled").trim();
  if (selected === "disabled") return new DisabledPaymentProvider();
  if (selected === "telegram_stars") {
    return new TelegramStarsPaymentProvider(
      (Deno.env.get("TELEGRAM_BOT_TOKEN") ?? "").trim(),
    );
  }
  throw new Error(`Unsupported PAYMENT_PROVIDER: ${selected}`);
}
