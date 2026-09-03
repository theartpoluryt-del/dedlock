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
import {
  commandFromText,
  InlineButton,
  Locale,
  navigationKeyboard,
  normalizeLocale,
  Section,
} from "./ui.ts";

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
const defaultDownloadUrl =
  "https://github.com/theartpoluryt-del/dedlock/raw/refs/heads/main/Dll6/x64/Release/AxiomLauncher.exe";

function requiredSecret(name: string): string {
  const value = (Deno.env.get(name) ?? "").trim();
  if (!value) throw new Error(`Missing secret: ${name}`);
  return value;
}

function configuredUrl(name: string, fallback: string): string {
  const value = (Deno.env.get(name) ?? fallback).trim();
  const url = new URL(value);
  if (!["https:", "http:", "tg:"].includes(url.protocol)) {
    throw new Error(`Unsupported URL protocol in ${name}`);
  }
  return url.toString();
}

function tr(locale: Locale, ru: string, en: string): string {
  return locale === "en" ? en : ru;
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

function formatMoney(
  amountMinor: number,
  currency: string,
  locale: Locale,
): string {
  return new Intl.NumberFormat(locale === "en" ? "en-US" : "ru-RU", {
    style: "currency",
    currency,
  }).format(amountMinor / 100);
}

function formatDate(value: unknown, locale: Locale): string {
  return new Intl.DateTimeFormat(locale === "en" ? "en-US" : "ru-RU", {
    dateStyle: "medium",
    timeStyle: "short",
    timeZone: Deno.env.get("DISPLAY_TIME_ZONE") ?? "UTC",
  }).format(new Date(String(value)));
}

function planTitle(plan: Plan, locale: Locale): string {
  if (locale === "ru") return plan.title;
  return `${plan.duration_days} ${plan.duration_days === 1 ? "day" : "days"}`;
}

function escapeHtml(value: unknown): string {
  return String(value ?? "").replaceAll("&", "&amp;").replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;").replaceAll('"', "&quot;");
}

async function sendMessage(
  chatId: number | string,
  text: string,
  extra: Record<string, unknown> = {},
): Promise<void> {
  await telegramCall("sendMessage", {
    chat_id: chatId,
    text,
    parse_mode: "HTML",
    disable_web_page_preview: true,
    ...extra,
  });
}

function controls(
  locale: Locale,
  section: Section,
  primary: InlineButton[][] = [],
): Record<string, unknown> {
  return {
    reply_markup: {
      inline_keyboard: [...primary, ...navigationKeyboard(locale, section)],
    },
  };
}

async function sendUserMessage(
  chatId: number,
  text: string,
  locale: Locale,
  section: Section,
  primary: InlineButton[][] = [],
): Promise<void> {
  await sendMessage(chatId, text, controls(locale, section, primary));
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

async function loadLocale(
  supabase: SupabaseClient,
  telegramUserId: number,
): Promise<Locale> {
  const { data, error } = await supabase.from("axiom_users")
    .select("language_code").eq("telegram_user_id", telegramUserId).single();
  if (error) throw error;
  return normalizeLocale(data.language_code);
}

async function setLocale(
  supabase: SupabaseClient,
  telegramUserId: number,
  locale: Locale,
): Promise<void> {
  const { error } = await supabase.rpc("axiom_set_telegram_language", {
    p_telegram_user_id: telegramUserId,
    p_language_code: locale,
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

async function showMenu(chatId: number, locale: Locale): Promise<void> {
  await sendUserMessage(
    chatId,
    tr(
      locale,
      "<b>Axiom Deadlock Bot</b>\n\nВыберите нужный раздел:",
      "<b>Axiom Deadlock Bot</b>\n\nChoose a section:",
    ),
    locale,
    "menu",
  );
}

async function showBuy(
  supabase: SupabaseClient,
  chatId: number,
  locale: Locale,
): Promise<void> {
  const plans = await loadPlans(supabase);
  const planButtons = plans.map((plan) => [{
    text: `${planTitle(plan, locale)} — ${
      formatMoney(plan.amount_minor, plan.currency, locale)
    }`,
    callback_data: `buy:${plan.code}`,
  }]);
  await sendUserMessage(
    chatId,
    tr(
      locale,
      "<b>💲 Купить подписку</b>\n\nВыберите период подписки:",
      "<b>💲 Buy a subscription</b>\n\nChoose a subscription period:",
    ),
    locale,
    "buy",
    planButtons,
  );
}

async function showDownload(chatId: number, locale: Locale): Promise<void> {
  const url = configuredUrl("AXIOM_DOWNLOAD_URL", defaultDownloadUrl);
  await sendUserMessage(
    chatId,
    tr(
      locale,
      "<b>⬇️ Скачать Axiom Launcher</b>\n\nНажмите кнопку ниже, сохраните <code>AxiomLauncher.exe</code> и следуйте инструкции по запуску.",
      "<b>⬇️ Download Axiom Launcher</b>\n\nUse the button below, save <code>AxiomLauncher.exe</code>, then follow the launch guide.",
    ),
    locale,
    "download",
    [[{
      text: tr(
        locale,
        "⬇️ Скачать AxiomLauncher.exe",
        "⬇️ Download AxiomLauncher.exe",
      ),
      url,
    }]],
  );
}

async function showLanguage(chatId: number, locale: Locale): Promise<void> {
  await sendUserMessage(
    chatId,
    tr(locale, "<b>🌐 Выберите язык</b>", "<b>🌐 Choose your language</b>"),
    locale,
    "language",
    [[
      { text: "🇷🇺 Русский", callback_data: "lang:ru" },
      { text: "🇺🇸 English", callback_data: "lang:en" },
    ]],
  );
}

async function showGuide(chatId: number, locale: Locale): Promise<void> {
  await sendUserMessage(
    chatId,
    tr(
      locale,
      "<b>📖 Как запустить Axiom</b>\n\n1. Получите ключ через /trial или /buy.\n2. Скачайте <code>AxiomLauncher.exe</code> через /download.\n3. Запустите загрузчик и вставьте лицензионный ключ.\n4. Дождитесь проверки лицензии и загрузки актуальной версии.\n\nНе передавайте ключ другим пользователям.",
      "<b>📖 How to launch Axiom</b>\n\n1. Get a key using /trial or /buy.\n2. Download <code>AxiomLauncher.exe</code> using /download.\n3. Run the launcher and enter your license key.\n4. Wait for license verification and the latest version to load.\n\nDo not share your key with other users.",
    ),
    locale,
    "guide",
  );
}

async function showSupport(chatId: number, locale: Locale): Promise<void> {
  const fallback = `tg://user?id=${requiredSecret("TELEGRAM_ADMIN_CHAT_ID")}`;
  const url = configuredUrl("AXIOM_SUPPORT_URL", fallback);
  await sendUserMessage(
    chatId,
    tr(
      locale,
      "<b>🛟 Поддержка</b>\n\nОпишите проблему, приложите скриншот и укажите номер заказа или последние 5 символов ключа. Никому не отправляйте ключ целиком.",
      "<b>🛟 Support</b>\n\nDescribe the issue, attach a screenshot, and include your order number or the last 5 characters of the key. Never send the full key.",
    ),
    locale,
    "support",
    [[{
      text: tr(locale, "🛟 Написать в поддержку", "🛟 Contact support"),
      url,
    }]],
  );
}

async function showTrialWarning(chatId: number, locale: Locale): Promise<void> {
  await sendUserMessage(
    chatId,
    tr(
      locale,
      "<b>⚠️ Перед активацией Trial</b>\n\nПробный период действует <b>3 дня</b> с момента подтверждения и доступен только один раз для одного Telegram-аккаунта. Отсчёт начнётся сразу после нажатия кнопки ниже.\n\nСначала рекомендуем скачать и подготовить Axiom Launcher.",
      "<b>⚠️ Before activating the Trial</b>\n\nThe trial lasts for <b>3 days</b> from confirmation and can only be claimed once per Telegram account. The countdown starts immediately after you press the button below.\n\nWe recommend downloading and preparing Axiom Launcher first.",
    ),
    locale,
    "trial",
    [[{
      text: tr(locale, "✅ Активировать Trial", "✅ Activate Trial"),
      callback_data: "trial:confirm",
    }]],
  );
}

async function issueTrial(
  supabase: SupabaseClient,
  user: TelegramUser,
  chatId: number,
  locale: Locale,
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
    ? tr(locale, "🎁 Тестовый период активирован", "🎁 Trial activated")
    : tr(
      locale,
      "🎁 Тестовый период уже был активирован",
      "🎁 Trial was already activated",
    );
  await sendUserMessage(
    chatId,
    `<b>${heading}</b>\n\n${tr(locale, "Ключ", "Key")}: <code>${
      escapeHtml(deliveredKey)
    }</code>\n${tr(locale, "Действует до", "Valid until")}: ${
      escapeHtml(formatDate(data.expires_at, locale))
    }`,
    locale,
    "trial",
  );
}

async function showLicenses(
  supabase: SupabaseClient,
  user: TelegramUser,
  chatId: number,
  locale: Locale,
): Promise<void> {
  const { data: owner, error: ownerError } = await supabase.from("axiom_users")
    .select("id").eq("telegram_user_id", user.id).maybeSingle();
  if (ownerError) throw ownerError;
  if (!owner) {
    return await sendUserMessage(
      chatId,
      tr(locale, "У вас пока нет ключей.", "You do not have any keys yet."),
      locale,
      "keys",
    );
  }
  const { data, error } = await supabase.from("axiom_licenses")
    .select("key_ciphertext,expires_at,enabled,source_kind,created_at")
    .eq("user_id", owner.id).order("created_at", { ascending: false });
  if (error) throw error;
  if (!data.length) {
    return await sendUserMessage(
      chatId,
      tr(locale, "У вас пока нет ключей.", "You do not have any keys yet."),
      locale,
      "keys",
    );
  }
  const encryptionKey = requiredSecret("BOT_KEY_ENCRYPTION_KEY");
  const lines: string[] = [
    tr(locale, "<b>🔑 Ваши ключи</b>", "<b>🔑 Your keys</b>"),
  ];
  for (const row of data) {
    if (!row.key_ciphertext) continue;
    const key = await decryptLicense(row.key_ciphertext, encryptionKey);
    const active = row.enabled &&
      (!row.expires_at || new Date(row.expires_at) > new Date());
    lines.push(
      `\n${active ? "✅" : "⛔"} <code>${escapeHtml(key)}</code>\n${
        tr(locale, "до", "until")
      } ${escapeHtml(formatDate(row.expires_at, locale))}`,
    );
  }
  await sendUserMessage(chatId, lines.join("\n"), locale, "keys");
}

async function createOrder(
  supabase: SupabaseClient,
  provider: PaymentProvider,
  planCode: string,
  updateId: number,
  user: TelegramUser,
  chatId: number,
  locale: Locale,
): Promise<void> {
  const plans = await loadPlans(supabase);
  const plan = plans.find((candidate) => candidate.code === planCode);
  if (!plan) {
    return await sendUserMessage(
      chatId,
      tr(
        locale,
        "Этот тариф сейчас недоступен.",
        "This plan is currently unavailable.",
      ),
      locale,
      "buy",
    );
  }
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
      title: planTitle(plan, locale),
      amountMinor: order.amount_minor,
      currency: order.currency,
      telegramUserId: user.id,
    });
    await supabase.from("axiom_orders").update({
      provider_checkout_id: checkout.externalCheckoutId,
      updated_at: new Date().toISOString(),
    }).eq("id", order.id).eq("status", "pending_payment");
    await sendUserMessage(
      chatId,
      `<b>${tr(locale, "Заказ", "Order")} ${escapeHtml(order.id)}</b>\n${
        escapeHtml(planTitle(plan, locale))
      } — ${
        escapeHtml(formatMoney(order.amount_minor, order.currency, locale))
      }`,
      locale,
      "buy",
      [[{ text: tr(locale, "💳 Оплатить", "💳 Pay"), url: checkout.url }]],
    );
  } catch (error) {
    if (!(error instanceof PaymentUnavailableError)) throw error;
    await sendUserMessage(
      chatId,
      tr(
        locale,
        `Заказ <code>${
          escapeHtml(order.id)
        }</code> создан, но приём платежей пока не настроен. Оплата и выдача ключа не выполнялись.`,
        `Order <code>${
          escapeHtml(order.id)
        }</code> was created, but payment processing is not configured yet. No payment or key issuance occurred.`,
      ),
      locale,
      "buy",
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
    .select("telegram_user_id,username,language_code").eq("id", order.user_id)
    .single();
  if (error) throw error;
  const locale = normalizeLocale(user.language_code);
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
          formatMoney(Number(order.amount_minor), String(order.currency), "ru"),
        )
      }\nOrder ID: <code>${escapeHtml(order.id)}</code>\nКлюч: <code>${
        escapeHtml(key)
      }</code>\nДействует до: ${
        escapeHtml(formatDate(license.expires_at, "ru"))
      }`,
    );
    await supabase.from("axiom_orders").update({
      admin_notified_at: new Date().toISOString(),
    }).eq("id", order.id).is("admin_notified_at", null);
  }
  if (!order.buyer_notified_at) {
    await sendUserMessage(
      Number(user.telegram_user_id),
      `<b>${tr(locale, "Оплата подтверждена", "Payment confirmed")}</b>\n\n${
        tr(locale, "Ключ", "Key")
      }: <code>${escapeHtml(key)}</code>\n${
        tr(locale, "Действует до", "Valid until")
      }: ${escapeHtml(formatDate(license.expires_at, locale))}`,
      locale,
      "keys",
    );
    await supabase.from("axiom_orders").update({
      buyer_notified_at: new Date().toISOString(),
    }).eq("id", order.id).is("buyer_notified_at", null);
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

  let locale = await loadLocale(supabase, user.id);
  const callbackAction = callback?.data;
  const command = commandFromText(message?.text);

  if (callbackAction?.startsWith("lang:")) {
    locale = normalizeLocale(callbackAction.slice(5));
    await setLocale(supabase, user.id, locale);
    await sendUserMessage(
      chatId,
      tr(
        locale,
        "✅ Язык изменён на русский.",
        "✅ Language changed to English.",
      ),
      locale,
      "language",
    );
  } else if (callbackAction?.startsWith("buy:")) {
    await createOrder(
      supabase,
      provider,
      callbackAction.slice(4),
      update.update_id,
      user,
      chatId,
      locale,
    );
  } else {
    const action = callbackAction?.startsWith("nav:")
      ? callbackAction.slice(4)
      : callbackAction === "licenses"
      ? "keys"
      : callbackAction ?? command ?? "menu";
    if (action === "download") await showDownload(chatId, locale);
    else if (action === "buy") await showBuy(supabase, chatId, locale);
    else if (action === "language") await showLanguage(chatId, locale);
    else if (action === "keys") {
      await showLicenses(supabase, user, chatId, locale);
    } else if (action === "trial:confirm") {
      await issueTrial(supabase, user, chatId, locale);
    } else if (action === "trial") {
      await showTrialWarning(chatId, locale);
    } else if (action === "guide") await showGuide(chatId, locale);
    else if (action === "support") await showSupport(chatId, locale);
    else await showMenu(chatId, locale);
  }
  return new Response("ok");
}

Deno.serve(async (request) => {
  try {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname.endsWith("/health")) {
      return Response.json({
        ok: true,
        payment_provider: Deno.env.get("PAYMENT_PROVIDER") ?? "disabled",
        locales: ["ru", "en"],
      });
    }
    if (request.method !== "POST") {
      return new Response("method not allowed", { status: 405 });
    }
    const supabase = createClient(
      requiredSecret("SUPABASE_URL"),
      requiredSecret("SUPABASE_SERVICE_ROLE_KEY"),
      { auth: { persistSession: false, autoRefreshToken: false } },
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
