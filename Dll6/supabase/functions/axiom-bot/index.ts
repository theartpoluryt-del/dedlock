import { createClient, SupabaseClient } from "@supabase/supabase-js";
import {
  decryptLicense,
  digestLicense,
  encryptLicense,
  generateLicenseKey,
} from "./license.ts";
import {
  configuredPaymentProvider,
  PaymentProvider,
  PaymentUnavailableError,
  VerifiedPayment,
} from "./payment.ts";

type TelegramUser = {
  id: number;
  username?: string;
  first_name?: string;
  last_name?: string;
};

type TelegramMessage = {
  chat: { id: number };
  from?: TelegramUser;
  text?: string;
};
type CallbackQuery = {
  id: string;
  from: TelegramUser;
  data?: string;
  message?: TelegramMessage;
};
type TelegramUpdate = {
  update_id: number;
  message?: TelegramMessage;
  callback_query?: CallbackQuery;
};

type Plan = {
  code: string;
  title: string;
  duration_days: number;
  amount_minor: number;
  currency: string;
};

type Fulfillment = {
  accepted?: boolean;
  idempotent?: boolean;
  reason?: string;
  order?: Record<string, unknown>;
  license?: Record<string, unknown>;
};

const jsonHeaders = { "content-type": "application/json; charset=utf-8" };

function requiredSecret(name: string): string {
  const value = (Deno.env.get(name) ?? "").trim();
  if (!value) throw new Error(`Missing secret: ${name}`);
  return value;
}

function telegramApi(
  method: string,
  body: Record<string, unknown>,
): Promise<Response> {
  return fetch(
    `https://api.telegram.org/bot${
      requiredSecret("TELEGRAM_BOT_TOKEN")
    }/${method}`,
    {
      method: "POST",
      headers: jsonHeaders,
      body: JSON.stringify(body),
    },
  );
}

async function telegramCall(
  method: string,
  body: Record<string, unknown>,
): Promise<unknown> {
  const response = await telegramApi(method, body);
  const payload = await response.json();
  if (!response.ok || !payload.ok) throw new Error(`Telegram ${method} failed`);
  return payload.result;
}

function formatMoney(amountMinor: number, currency: string): string {
  return new Intl.NumberFormat("ru-RU", { style: "currency", currency })
    .format(amountMinor / 100);
}

function formatDate(value: unknown): string {
  return new Intl.DateTimeFormat("ru-RU", {
    dateStyle: "medium",
    timeStyle: "short",
    timeZone: Deno.env.get("DISPLAY_TIME_ZONE") ?? "UTC",
  }).format(new Date(String(value)));
}

function escapeHtml(value: unknown): string {
  return String(value ?? "").replaceAll("&", "&amp;").replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;").replaceAll('"', "&quot;");
}

async function sendMessage(
  chatId: number | string,
  text: string,
  extra = {},
): Promise<void> {
  await telegramCall("sendMessage", {
    chat_id: chatId,
    text,
    parse_mode: "HTML",
    disable_web_page_preview: true,
    ...extra,
  });
}

async function upsertUser(
  supabase: SupabaseClient,
  user: TelegramUser,
): Promise<void> {
  const { error } = await supabase.rpc("axiom_upsert_telegram_user", {
    p_telegram_user_id: user.id,
    p_username: user.username ?? null,
    p_first_name: user.first_name ?? null,
    p_last_name: user.last_name ?? null,
  });
  if (error) throw error;
}

async function loadPlans(supabase: SupabaseClient): Promise<Plan[]> {
  const { data, error } = await supabase.from("axiom_plans")
    .select("code,title,duration_days,amount_minor,currency")
    .eq("active", true).order("sort_order");
  if (error) throw error;
  return data as Plan[];
}

async function showMenu(
  supabase: SupabaseClient,
  chatId: number,
): Promise<void> {
  const plans = await loadPlans(supabase);
  const keyboard = plans.map((plan) => [{
    text: `${plan.title} — ${formatMoney(plan.amount_minor, plan.currency)}`,
    callback_data: `buy:${plan.code}`,
  }]);
  keyboard.push([{ text: "🎁 Бесплатные 3 дня", callback_data: "trial" }]);
  keyboard.push([{ text: "🔑 Мои лицензии", callback_data: "licenses" }]);
  await sendMessage(
    chatId,
    "<b>Axiom Launcher</b>\n\nВыберите период подписки или активируйте один бесплатный тестовый период:",
    { reply_markup: { inline_keyboard: keyboard } },
  );
}

async function issueTrial(
  supabase: SupabaseClient,
  user: TelegramUser,
  chatId: number,
): Promise<void> {
  const key = generateLicenseKey();
  const pepper = requiredSecret("LICENSE_PEPPER");
  const encryptionKey = requiredSecret("BOT_KEY_ENCRYPTION_KEY");
  const { data, error } = await supabase.rpc("axiom_claim_trial", {
    p_telegram_user_id: user.id,
    p_key_hash: await digestLicense(key, pepper),
    p_key_ciphertext: await encryptLicense(key, encryptionKey),
  });
  if (error) throw error;
  const deliveredKey = data.created
    ? key
    : await decryptLicense(data.key_ciphertext, encryptionKey);
  const heading = data.created
    ? "Тестовый период активирован"
    : "Тестовый период уже был активирован";
  await sendMessage(
    chatId,
    `<b>${heading}</b>\n\nКлюч: <code>${
      escapeHtml(deliveredKey)
    }</code>\nДействует до: ${escapeHtml(formatDate(data.expires_at))}`,
  );
}

async function showLicenses(
  supabase: SupabaseClient,
  user: TelegramUser,
  chatId: number,
): Promise<void> {
  const { data: owner, error: ownerError } = await supabase.from("axiom_users")
    .select("id").eq("telegram_user_id", user.id).maybeSingle();
  if (ownerError) throw ownerError;
  if (!owner) return await sendMessage(chatId, "Лицензий пока нет.");
  const { data, error } = await supabase.from("axiom_licenses")
    .select("key_ciphertext,expires_at,enabled,source_kind,created_at")
    .eq("user_id", owner.id).order("created_at", { ascending: false });
  if (error) throw error;
  if (!data.length) return await sendMessage(chatId, "Лицензий пока нет.");
  const encryptionKey = requiredSecret("BOT_KEY_ENCRYPTION_KEY");
  const lines: string[] = ["<b>Ваши лицензии</b>"];
  for (const row of data) {
    if (!row.key_ciphertext) continue;
    const key = await decryptLicense(row.key_ciphertext, encryptionKey);
    const active = row.enabled &&
      (!row.expires_at || new Date(row.expires_at) > new Date());
    lines.push(
      `\n${active ? "✅" : "⛔"} <code>${escapeHtml(key)}</code>\nдо ${
        escapeHtml(formatDate(row.expires_at))
      }`,
    );
  }
  await sendMessage(chatId, lines.join("\n"));
}

async function createOrder(
  supabase: SupabaseClient,
  provider: PaymentProvider,
  planCode: string,
  updateId: number,
  user: TelegramUser,
  chatId: number,
): Promise<void> {
  const plans = await loadPlans(supabase);
  const plan = plans.find((candidate) => candidate.code === planCode);
  if (!plan) return await sendMessage(chatId, "Этот тариф сейчас недоступен.");
  const { data: order, error } = await supabase.rpc("axiom_create_order", {
    p_telegram_user_id: user.id,
    p_plan_code: planCode,
    p_provider: provider.name,
    p_telegram_update_id: updateId,
  });
  if (error) throw error;
  try {
    const checkout = await provider.createCheckout({
      id: order.id,
      title: plan.title,
      amountMinor: order.amount_minor,
      currency: order.currency,
      telegramUserId: user.id,
    });
    await supabase.from("axiom_orders").update({
      provider_checkout_id: checkout.externalCheckoutId,
      updated_at: new Date().toISOString(),
    }).eq("id", order.id).eq("status", "pending_payment");
    await sendMessage(
      chatId,
      `<b>Заказ ${escapeHtml(order.id)}</b>\n${escapeHtml(plan.title)} — ${
        escapeHtml(formatMoney(order.amount_minor, order.currency))
      }`,
      {
        reply_markup: {
          inline_keyboard: [[{ text: "Оплатить", url: checkout.url }]],
        },
      },
    );
  } catch (error) {
    if (!(error instanceof PaymentUnavailableError)) throw error;
    await sendMessage(
      chatId,
      `Заказ <code>${
        escapeHtml(order.id)
      }</code> создан, но приём платежей пока не настроен. Оплата и выдача ключа не выполнялись.`,
    );
  }
}

async function notifyPurchase(
  supabase: SupabaseClient,
  fulfillment: Fulfillment,
): Promise<void> {
  const order = fulfillment.order!;
  const license = fulfillment.license!;
  const { data: user, error } = await supabase.from("axiom_users")
    .select("telegram_user_id,username").eq("id", order.user_id).single();
  if (error) throw error;
  const { data: plan, error: planError } = await supabase.from("axiom_plans")
    .select("title").eq("code", order.plan_code).single();
  if (planError) throw planError;
  const key = await decryptLicense(
    String(license.key_ciphertext),
    requiredSecret("BOT_KEY_ENCRYPTION_KEY"),
  );
  const username = user.username ? `@${escapeHtml(user.username)}` : "—";
  if (!order.admin_notified_at) {
    await sendMessage(
      requiredSecret("TELEGRAM_ADMIN_CHAT_ID"),
      `<b>Новая покупка Axiom</b>\nTelegram: <code>${user.telegram_user_id}</code> (${username})\nТариф: ${
        escapeHtml(plan.title)
      } (${escapeHtml(order.plan_code)})\nСумма: ${
        escapeHtml(
          formatMoney(Number(order.amount_minor), String(order.currency)),
        )
      }\nOrder ID: <code>${escapeHtml(order.id)}</code>\nКлюч: <code>${
        escapeHtml(key)
      }</code>\nДействует до: ${escapeHtml(formatDate(license.expires_at))}`,
    );
    await supabase.from("axiom_orders").update({
      admin_notified_at: new Date().toISOString(),
    })
      .eq("id", order.id).is("admin_notified_at", null);
  }
  if (!order.buyer_notified_at) {
    await sendMessage(
      user.telegram_user_id,
      `<b>Оплата подтверждена</b>\n\nКлюч: <code>${
        escapeHtml(key)
      }</code>\nДействует до: ${escapeHtml(formatDate(license.expires_at))}`,
    );
    await supabase.from("axiom_orders").update({
      buyer_notified_at: new Date().toISOString(),
    })
      .eq("id", order.id).is("buyer_notified_at", null);
  }
}

async function fulfillPayment(
  supabase: SupabaseClient,
  payment: VerifiedPayment,
): Promise<Fulfillment> {
  const key = generateLicenseKey();
  const { data, error } = await supabase.rpc("axiom_fulfill_paid_order", {
    p_order_id: payment.orderId,
    p_provider: payment.provider,
    p_provider_event_id: payment.eventId,
    p_external_payment_id: payment.externalPaymentId,
    p_amount_minor: payment.amountMinor,
    p_currency: payment.currency.toUpperCase(),
    p_key_hash: await digestLicense(key, requiredSecret("LICENSE_PEPPER")),
    p_key_ciphertext: await encryptLicense(
      key,
      requiredSecret("BOT_KEY_ENCRYPTION_KEY"),
    ),
  });
  if (error) throw error;
  return data as Fulfillment;
}

async function handleTelegram(
  request: Request,
  supabase: SupabaseClient,
  provider: PaymentProvider,
): Promise<Response> {
  const expected = requiredSecret("TELEGRAM_WEBHOOK_SECRET");
  if (request.headers.get("x-telegram-bot-api-secret-token") !== expected) {
    return new Response("unauthorized", { status: 401 });
  }
  const update = await request.json() as TelegramUpdate;
  const callback = update.callback_query;
  const message = update.message;
  const user = callback?.from ?? message?.from;
  const chatId = callback?.message?.chat.id ?? message?.chat.id;
  if (!user || !chatId) return new Response("ok");
  await upsertUser(supabase, user);
  if (callback) {
    await telegramCall("answerCallbackQuery", {
      callback_query_id: callback.id,
    });
  }

  const action = callback?.data ?? message?.text ?? "/start";
  if (action === "trial") await issueTrial(supabase, user, chatId);
  else if (action === "licenses") await showLicenses(supabase, user, chatId);
  else if (action.startsWith("buy:")) {
    await createOrder(
      supabase,
      provider,
      action.slice(4),
      update.update_id,
      user,
      chatId,
    );
  } else await showMenu(supabase, chatId);
  return new Response("ok");
}

Deno.serve(async (request) => {
  try {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname.endsWith("/health")) {
      return Response.json({
        ok: true,
        payment_provider: Deno.env.get("PAYMENT_PROVIDER") ?? "disabled",
      });
    }
    if (request.method !== "POST") {
      return new Response("method not allowed", { status: 405 });
    }
    const supabase = createClient(
      requiredSecret("SUPABASE_URL"),
      requiredSecret("SUPABASE_SERVICE_ROLE_KEY"),
      {
        auth: { persistSession: false, autoRefreshToken: false },
      },
    );
    const provider = configuredPaymentProvider();
    if (url.pathname.endsWith("/telegram")) {
      return await handleTelegram(request, supabase, provider);
    }
    if (url.pathname.endsWith(`/payments/${provider.name}`)) {
      const fulfillment = await fulfillPayment(
        supabase,
        await provider.verifyWebhook(request),
      );
      if (fulfillment.accepted) await notifyPurchase(supabase, fulfillment);
      return Response.json({
        ok: true,
        accepted: fulfillment.accepted ?? true,
      });
    }
    return new Response("not found", { status: 404 });
  } catch (error) {
    console.error(error instanceof Error ? error.message : error);
    const status = error instanceof PaymentUnavailableError ? 503 : 500;
    return Response.json({ ok: false }, { status });
  }
});
