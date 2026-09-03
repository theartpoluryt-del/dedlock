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
  throw new Error(`Unsupported PAYMENT_PROVIDER: ${selected}`);
}
