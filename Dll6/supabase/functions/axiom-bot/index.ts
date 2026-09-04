import { createClient, SupabaseClient } from "@supabase/supabase-js";
import {
  decryptLicense,
  digestActivationCode,
  digestLicense,
  encryptLicense,
  generateLicenseKey,
  normalizeActivationCode,
} from "./license.ts";
import {
  configuredPaymentProvider,
  orderIdFromTelegramPayload,
  PaymentProvider,
  PaymentUnavailableError,
  rubMinorToStars,
  VerifiedPayment,
} from "./payment.ts";
import {
  commandDefinitions,
  commandFromText,
  InlineButton,
  Locale,
  navigationKeyboard,
  normalizeLocale,
  Section,
} from "./ui.ts";
import {
  parseAdminIds,
  SupportContent,
  supportContent,
  ticketIdFromCallback,
} from "./support.ts";

type TelegramUser = {
  id: number;
  username?: string;
  first_name?: string;
  last_name?: string;
};

type TelegramMessage = {
  message_id: number;
  chat: { id: number; type?: string };
  from?: TelegramUser;
  text?: string;
  caption?: string;
  photo?: Array<{ file_id: string; file_size?: number }>;
  document?: { file_id: string; file_name?: string; mime_type?: string };
  successful_payment?: {
    currency: string;
    total_amount: number;
    invoice_payload: string;
    telegram_payment_charge_id: string;
    provider_payment_charge_id?: string;
  };
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
  pre_checkout_query?: {
    id: string;
    from: TelegramUser;
    currency: string;
    total_amount: number;
    invoice_payload: string;
  };
};

type Plan = {
  code: string;
  title: string;
  duration_days: number;
  amount_minor: number;
  currency: string;
  purchase_url: string | null;
};

type StarSettings = {
  rub_per_star: number | string;
};

type Fulfillment = {
  accepted?: boolean;
  idempotent?: boolean;
  reason?: string;
  order?: Record<string, unknown>;
  license?: Record<string, unknown>;
};

type ActivationFulfillment = {
  accepted: boolean;
  idempotent?: boolean;
  created_license?: boolean;
  reason?: string;
  activation?: Record<string, unknown>;
  license?: Record<string, unknown>;
};

const jsonHeaders = { "content-type": "application/json; charset=utf-8" };
const defaultDownloadUrl =
  "https://github.com/theartpoluryt-del/dedlock/raw/refs/heads/main/Dll6/x64/Release/AxiomLauncher.exe";
let telegramWebhookConfiguration: Promise<void> | null = null;
let supportWebhookConfiguration: Promise<void> | null = null;

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

function botApi(
  token: string,
  method: string,
  body: Record<string, unknown>,
): Promise<Response> {
  return fetch(
    `https://api.telegram.org/bot${token}/${method}`,
    {
      method: "POST",
      headers: jsonHeaders,
      body: JSON.stringify(body),
    },
  );
}

async function botCall(
  token: string,
  method: string,
  body: Record<string, unknown>,
): Promise<unknown> {
  const response = await botApi(token, method, body);
  const payload = await response.json();
  if (!response.ok || !payload.ok) {
    const description = typeof payload.description === "string"
      ? payload.description
      : "unknown Telegram error";
    throw new Error(
      `Telegram ${method} failed (${response.status}): ${description}`,
    );
  }
  return payload.result;
}

function telegramCall(
  method: string,
  body: Record<string, unknown>,
): Promise<unknown> {
  return botCall(requiredSecret("TELEGRAM_BOT_TOKEN"), method, body);
}

function supportTelegramCall(
  method: string,
  body: Record<string, unknown>,
): Promise<unknown> {
  return botCall(requiredSecret("SUPPORT_BOT_TOKEN"), method, body);
}

function ensureTelegramWebhook(): Promise<void> {
  if (!telegramWebhookConfiguration) {
    const baseUrl = requiredSecret("SUPABASE_URL").replace(/\/$/, "");
    telegramWebhookConfiguration = Promise.all([
      telegramCall("setWebhook", {
        url: `${baseUrl}/functions/v1/axiom-bot/telegram`,
        secret_token: requiredSecret("TELEGRAM_WEBHOOK_SECRET"),
        allowed_updates: ["message", "callback_query", "pre_checkout_query"],
      }),
      telegramCall("setMyCommands", { commands: commandDefinitions("ru") }),
      telegramCall("setMyCommands", {
        commands: commandDefinitions("ru"),
        language_code: "ru",
      }),
      telegramCall("setMyCommands", {
        commands: commandDefinitions("en"),
        language_code: "en",
      }),
    ]).then(() => undefined).catch((error) => {
      telegramWebhookConfiguration = null;
      throw error;
    });
  }
  return telegramWebhookConfiguration;
}

function supportConfigured(): boolean {
  return Boolean(
    (Deno.env.get("SUPPORT_BOT_TOKEN") ?? "").trim() &&
      (Deno.env.get("SUPPORT_WEBHOOK_SECRET") ?? "").trim(),
  );
}

function supportAdminIds(): number[] {
  return parseAdminIds(
    Deno.env.get("SUPPORT_ADMIN_IDS"),
    requiredSecret("TELEGRAM_ADMIN_CHAT_ID"),
  );
}

function ensureSupportWebhook(): Promise<void> {
  if (!supportConfigured()) return Promise.resolve();
  if (!supportWebhookConfiguration) {
    const baseUrl = requiredSecret("SUPABASE_URL").replace(/\/$/, "");
    supportWebhookConfiguration = Promise.all([
      supportTelegramCall("setWebhook", {
        url: `${baseUrl}/functions/v1/axiom-bot/support-telegram`,
        secret_token: requiredSecret("SUPPORT_WEBHOOK_SECRET"),
        allowed_updates: ["message", "callback_query"],
      }),
      supportTelegramCall("setMyCommands", {
        commands: [
          { command: "tickets", description: "📬 Открытые тикеты" },
          { command: "close", description: "✅ Закрыть активный тикет" },
          { command: "cancel", description: "⏸ Выйти из диалога" },
        ],
      }),
    ]).then(() => undefined).catch((error) => {
      supportWebhookConfiguration = null;
      throw error;
    });
  }
  return supportWebhookConfiguration;
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

function formatPaymentAmount(amount: number, currency: string): string {
  return currency === "XTR"
    ? `${amount} ⭐`
    : formatMoney(amount, currency, "ru");
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
    .select("code,title,duration_days,amount_minor,currency,purchase_url")
    .eq("active", true).order("sort_order");
  if (error) throw error;
  return data as Plan[];
}

async function loadRubPerStar(supabase: SupabaseClient): Promise<number> {
  const { data, error } = await supabase.from("axiom_payment_settings")
    .select("rub_per_star").eq("singleton", true).single();
  if (error) throw error;
  const rate = Number((data as StarSettings).rub_per_star);
  if (!Number.isFinite(rate) || rate <= 0) {
    throw new Error("Invalid Stars exchange rate");
  }
  return rate;
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
    callback_data: `offer:${plan.code}`,
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

async function showFunPayOffer(
  supabase: SupabaseClient,
  chatId: number,
  locale: Locale,
  planCode: string,
): Promise<void> {
  const plan = (await loadPlans(supabase)).find((item) =>
    item.code === planCode
  );
  if (!plan?.purchase_url) {
    return await sendUserMessage(
      chatId,
      tr(
        locale,
        "Этот тариф пока недоступен на FunPay. Попробуйте позже или обратитесь в поддержку.",
        "This plan is not available on FunPay yet. Try later or contact support.",
      ),
      locale,
      "buy",
    );
  }
  await sendUserMessage(
    chatId,
    `<b>${tr(locale, "Покупка через FunPay", "Purchase via FunPay")}</b>\n\n${
      tr(locale, "Тариф", "Plan")
    }: ${escapeHtml(planTitle(plan, locale))}\n${
      tr(locale, "Цена", "Price")
    }: ${
      escapeHtml(formatMoney(plan.amount_minor, plan.currency, locale))
    }\n\n${
      tr(
        locale,
        "1. Откройте предложение на FunPay и оплатите его.\n2. FunPay автоматически выдаст одноразовый код активации.\n3. Вернитесь в бот и отправьте <code>/activate КОД</code>.\n4. Бот выдаст отдельный лицензионный ключ Axiom.",
        "1. Open the FunPay offer and pay for it.\n2. FunPay will automatically deliver a one-time activation code.\n3. Return to the bot and send <code>/activate CODE</code>.\n4. The bot will issue a separate Axiom license key.",
      )
    }`,
    locale,
    "buy",
    [
      [{
        text: tr(locale, "🛒 Открыть FunPay", "🛒 Open FunPay"),
        url: plan.purchase_url,
      }],
    ],
  );
}

async function showActivate(chatId: number, locale: Locale): Promise<void> {
  await sendUserMessage(
    chatId,
    tr(
      locale,
      "<b>🔐 Активация покупки FunPay</b>\n\nПосле оплаты скопируйте код, выданный FunPay, и отправьте его так:\n<code>/activate K7M9R2W4H8NP3X6Q</code>\n\nКод одноразовый. Не передавайте его другим людям.",
      "<b>🔐 Activate a FunPay purchase</b>\n\nAfter payment, copy the code delivered by FunPay and send it like this:\n<code>/activate K7M9R2W4H8NP3X6Q</code>\n\nThe code is one-time. Do not share it.",
    ),
    locale,
    "activate",
  );
}

async function showDownload(chatId: number, locale: Locale): Promise<void> {
  try {
    await telegramCall("sendDocument", {
      chat_id: chatId,
      document: requiredSecret("AXIOM_LAUNCHER_FILE_ID"),
      caption: tr(
        locale,
        "<b>⬇️ Axiom Launcher</b>\n\nСкачайте прикреплённый файл и следуйте инструкции по запуску.",
        "<b>⬇️ Axiom Launcher</b>\n\nDownload the attached file and follow the launch guide.",
      ),
      parse_mode: "HTML",
      ...controls(locale, "download"),
    });
  } catch (error) {
    console.error(error instanceof Error ? error.message : error);
    await sendUserMessage(
      chatId,
      tr(
        locale,
        "<b>⚠️ Файл временно недоступен</b>\n\nПопробуйте /download немного позже.",
        "<b>⚠️ File temporarily unavailable</b>\n\nPlease try /download again later.",
      ),
      locale,
      "download",
    );
  }
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
      "<b>📖 Как запустить Axiom</b>\n\n1. Получите ключ через /trial или /buy.\n2. Скачайте <code>AxiomLauncher.exe</code> через /download.\n3. Запустите загрузчик и вставьте лицензионный ключ.\n4. Дождитесь проверки лицензии и загрузки актуальной версии.\n\n<b>🎛 Как добавить свой конфиг</b>\n\n1. Откройте меню клавишей <b>Insert</b> или <b>Page Up</b>.\n2. Настройте функции и нажмите <b>Save</b> в верхней части меню.\n3. Введите название конфига и нажмите <b>Enter</b>.\n4. Для загрузки нажмите на список <b>Select config</b> и выберите сохранённый конфиг.\n\nЧтобы импортировать готовый файл, поместите его с расширением <code>.ini</code> в папку <code>%LOCALAPPDATA%\\Axiom</code>, затем откройте список <b>Select config</b>.\n\nНе передавайте ключ другим пользователям.",
      "<b>📖 How to launch Axiom</b>\n\n1. Get a key using /trial or /buy.\n2. Download <code>AxiomLauncher.exe</code> using /download.\n3. Run the launcher and enter your license key.\n4. Wait for license verification and the latest version to load.\n\n<b>🎛 How to add your config</b>\n\n1. Open the menu with <b>Insert</b> or <b>Page Up</b>.\n2. Adjust the features and click <b>Save</b> at the top of the menu.\n3. Enter a config name and press <b>Enter</b>.\n4. To load it, open <b>Select config</b> and choose the saved config.\n\nTo import an existing config, place its <code>.ini</code> file in <code>%LOCALAPPDATA%\\Axiom</code>, then open <b>Select config</b>.\n\nDo not share your key with other users.",
    ),
    locale,
    "guide",
  );
}

type SupportTicket = {
  id: string;
  ticket_number: number;
  user_id: string;
  status: "waiting_admin" | "waiting_user" | "closed";
};

async function sendSupportMessage(
  chatId: number,
  text: string,
  extra: Record<string, unknown> = {},
): Promise<void> {
  await supportTelegramCall("sendMessage", {
    chat_id: chatId,
    text,
    parse_mode: "HTML",
    disable_web_page_preview: true,
    ...extra,
  });
}

function ticketKeyboard(ticketId: string): Record<string, unknown> {
  return {
    reply_markup: {
      inline_keyboard: [[
        { text: "💬 Открыть диалог", callback_data: `ticket:open:${ticketId}` },
        { text: "✅ Закрыть", callback_data: `ticket:close:${ticketId}` },
      ]],
    },
  };
}

async function ticketAndUser(
  supabase: SupabaseClient,
  ticketId: string,
): Promise<{ ticket: SupportTicket; user: Record<string, unknown> }> {
  const { data: ticket, error: ticketError } = await supabase
    .from("axiom_support_tickets")
    .select("id,ticket_number,user_id,status").eq("id", ticketId).single();
  if (ticketError) throw ticketError;
  const { data: user, error: userError } = await supabase.from("axiom_users")
    .select("telegram_user_id,username,first_name,last_name,language_code")
    .eq("id", ticket.user_id).single();
  if (userError) throw userError;
  return { ticket: ticket as SupportTicket, user };
}

function supportUserLabel(user: Record<string, unknown>): string {
  const username = user.username
    ? `@${escapeHtml(user.username)}`
    : "без username";
  const name = [user.first_name, user.last_name].filter(Boolean).map(escapeHtml)
    .join(" ");
  return `${name || "Без имени"} (${username})\nTelegram ID: <code>${
    escapeHtml(user.telegram_user_id)
  }</code>`;
}

async function notifyTicketCreated(
  supabase: SupabaseClient,
  ticketId: string,
): Promise<void> {
  if (!supportConfigured()) return;
  const { ticket, user } = await ticketAndUser(supabase, ticketId);
  await Promise.all(
    supportAdminIds().map((adminId) =>
      sendSupportMessage(
        adminId,
        `<b>🎫 Новый тикет #${ticket.ticket_number}</b>\n\n${
          supportUserLabel(user)
        }\n\nОжидаем первое сообщение пользователя.`,
        ticketKeyboard(ticket.id),
      ).catch((error) =>
        console.error("Support ticket notification failed", error)
      )
    ),
  );
}

async function openSupportTicket(
  supabase: SupabaseClient,
  telegramUserId: number,
): Promise<{ id: string; ticket_number: number; created: boolean }> {
  const { data, error } = await supabase.rpc("axiom_support_open_ticket", {
    p_telegram_user_id: telegramUserId,
  });
  if (error) throw error;
  return data;
}

async function activeUserTicket(
  supabase: SupabaseClient,
  telegramUserId: number,
): Promise<SupportTicket | null> {
  const { data: user, error: userError } = await supabase.from("axiom_users")
    .select("id").eq("telegram_user_id", telegramUserId).single();
  if (userError) throw userError;
  const { data: session, error: sessionError } = await supabase
    .from("axiom_support_user_sessions").select("active_ticket_id")
    .eq("user_id", user.id).maybeSingle();
  if (sessionError) throw sessionError;
  if (!session) return null;
  const { data: ticket, error: ticketError } = await supabase
    .from("axiom_support_tickets").select("id,ticket_number,user_id,status")
    .eq("id", session.active_ticket_id).neq("status", "closed").maybeSingle();
  if (ticketError) throw ticketError;
  return ticket as SupportTicket | null;
}

async function storeSupportMessage(
  supabase: SupabaseClient,
  ticketId: string,
  senderRole: "user" | "admin",
  sourceBot: "axiom" | "support",
  message: TelegramMessage,
  content: SupportContent,
): Promise<string | null> {
  const { data, error } = await supabase.from("axiom_support_messages").upsert({
    ticket_id: ticketId,
    sender_role: senderRole,
    sender_telegram_user_id: message.from!.id,
    body: content.body,
    content_type: content.type,
    source_bot: sourceBot,
    source_chat_id: message.chat.id,
    source_message_id: message.message_id,
    source_file_id: content.fileId,
    file_name: content.fileName,
    mime_type: content.mimeType,
  }, {
    onConflict: "source_bot,source_chat_id,source_message_id",
    ignoreDuplicates: true,
  }).select("id").maybeSingle();
  if (error) throw error;
  if (!data) return null;
  const nextStatus = senderRole === "user" ? "waiting_admin" : "waiting_user";
  const { error: updateError } = await supabase.from("axiom_support_tickets")
    .update({
      status: nextStatus,
      last_message_at: new Date().toISOString(),
      updated_at: new Date().toISOString(),
    }).eq("id", ticketId).neq("status", "closed");
  if (updateError) throw updateError;
  return data.id;
}

async function relaySupportMedia(
  sourceToken: string,
  targetToken: string,
  targetChatId: number,
  content: SupportContent,
): Promise<void> {
  if (!content.fileId || content.type === "text") return;
  const file = await botCall(sourceToken, "getFile", {
    file_id: content.fileId,
  }) as {
    file_path: string;
  };
  const downloaded = await fetch(
    `https://api.telegram.org/file/bot${sourceToken}/${file.file_path}`,
  );
  if (!downloaded.ok) throw new Error("Telegram attachment download failed");
  const form = new FormData();
  form.set("chat_id", String(targetChatId));
  if (content.body) form.set("caption", content.body.slice(0, 1024));
  const field = content.type === "photo" ? "photo" : "document";
  form.set(
    field,
    new File([await downloaded.blob()], content.fileName ?? "attachment", {
      type: content.mimeType ?? "application/octet-stream",
    }),
  );
  const response = await fetch(
    `https://api.telegram.org/bot${targetToken}/send${
      content.type === "photo" ? "Photo" : "Document"
    }`,
    { method: "POST", body: form },
  );
  if (!response.ok) throw new Error("Telegram attachment upload failed");
}

async function markSupportDelivery(
  supabase: SupabaseClient,
  messageId: string,
  error?: unknown,
): Promise<void> {
  const { error: updateError } = await supabase.from("axiom_support_messages")
    .update(
      error
        ? {
          delivery_status: "failed",
          delivery_error: String(error instanceof Error ? error.message : error)
            .slice(0, 500),
        }
        : {
          delivery_status: "delivered",
          delivered_at: new Date().toISOString(),
        },
    )
    .eq("id", messageId);
  if (updateError) {
    console.error("Support delivery status update failed", updateError);
  }
}

async function showSupport(
  supabase: SupabaseClient,
  user: TelegramUser,
  chatId: number,
  locale: Locale,
): Promise<void> {
  if (!supportConfigured()) {
    return await sendUserMessage(
      chatId,
      tr(
        locale,
        "🛠 Поддержка временно настраивается. Пожалуйста, попробуйте немного позже.",
        "🛠 Support is being configured. Please try again a little later.",
      ),
      locale,
      "support",
    );
  }
  const ticket = await openSupportTicket(supabase, user.id);
  if (ticket.created) await notifyTicketCreated(supabase, ticket.id);
  await sendUserMessage(
    chatId,
    tr(
      locale,
      `<b>🛟 Поддержка · тикет #${ticket.ticket_number}</b>\n\nТеперь отправьте сюда сообщение, скриншот или файл. Все следующие обычные сообщения будут передаваться оператору, пока вы не завершите диалог.\n\nУкажите номер заказа или последние 5 символов ключа. Не отправляйте ключ целиком.`,
      `<b>🛟 Support · ticket #${ticket.ticket_number}</b>\n\nSend a message, screenshot, or file here. Your following regular messages will be delivered to an operator until you end the conversation.\n\nInclude your order number or the last 5 characters of the key. Do not send the full key.`,
    ),
    locale,
    "support",
    [[{
      text: tr(locale, "✅ Завершить обращение", "✅ End conversation"),
      callback_data: "support:close",
    }]],
  );
}

async function handleUserSupportMessage(
  supabase: SupabaseClient,
  user: TelegramUser,
  message: TelegramMessage,
  locale: Locale,
): Promise<boolean> {
  const ticket = await activeUserTicket(supabase, user.id);
  if (!ticket) return false;
  const content = supportContent(message);
  if (!content) return false;
  const storedId = await storeSupportMessage(
    supabase,
    ticket.id,
    "user",
    "axiom",
    message,
    content,
  );
  if (!storedId) return true;

  const { user: storedUser } = await ticketAndUser(supabase, ticket.id);
  const body = content.body ? `\n\n${escapeHtml(content.body)}` : "";
  try {
    await Promise.all(
      supportAdminIds().map(async (adminId) => {
        await sendSupportMessage(
          adminId,
          `<b>📩 Тикет #${ticket.ticket_number}</b>\n${
            supportUserLabel(storedUser)
          }${body}`,
          ticketKeyboard(ticket.id),
        );
        await relaySupportMedia(
          requiredSecret("TELEGRAM_BOT_TOKEN"),
          requiredSecret("SUPPORT_BOT_TOKEN"),
          adminId,
          content,
        );
      }),
    );
    await markSupportDelivery(supabase, storedId);
    await sendUserMessage(
      message.chat.id,
      tr(
        locale,
        `✅ Сообщение доставлено оператору по тикету #${ticket.ticket_number}. Можете продолжать писать сюда.`,
        `✅ Your message was delivered for ticket #${ticket.ticket_number}. You can keep writing here.`,
      ),
      locale,
      "support",
      [[{
        text: tr(locale, "✅ Завершить обращение", "✅ End conversation"),
        callback_data: "support:close",
      }]],
    );
  } catch (error) {
    await markSupportDelivery(supabase, storedId, error);
    await sendUserMessage(
      message.chat.id,
      tr(
        locale,
        "⚠️ Не удалось доставить сообщение оператору. Оно сохранено в тикете; попробуйте ещё раз позже.",
        "⚠️ The message could not be delivered to the operator. It is saved in the ticket; try again later.",
      ),
      locale,
      "support",
    );
  }
  return true;
}

async function closeUserSupportTicket(
  supabase: SupabaseClient,
  user: TelegramUser,
  chatId: number,
  locale: Locale,
): Promise<void> {
  const ticket = await activeUserTicket(supabase, user.id);
  if (!ticket) {
    return await sendUserMessage(
      chatId,
      tr(
        locale,
        "У вас нет активного обращения.",
        "You have no active ticket.",
      ),
      locale,
      "support",
    );
  }
  const { error } = await supabase.rpc("axiom_support_close_ticket", {
    p_ticket_id: ticket.id,
    p_closed_by_telegram_user_id: user.id,
  });
  if (error) throw error;
  if (supportConfigured()) {
    await Promise.all(
      supportAdminIds().map((adminId) =>
        sendSupportMessage(
          adminId,
          `<b>✅ Тикет #${ticket.ticket_number} закрыт пользователем</b>`,
        ).catch((notifyError) =>
          console.error("Support close notification failed", notifyError)
        )
      ),
    );
  }
  await sendUserMessage(
    chatId,
    tr(
      locale,
      `✅ Обращение #${ticket.ticket_number} закрыто. Чтобы создать новое, снова нажмите «Поддержка».`,
      `✅ Ticket #${ticket.ticket_number} is closed. Select “Support” again to create a new one.`,
    ),
    locale,
    "support",
  );
}

async function openAdminTicket(
  supabase: SupabaseClient,
  adminId: number,
  ticketId: string,
): Promise<void> {
  const { ticket, user } = await ticketAndUser(supabase, ticketId);
  if (ticket.status === "closed") {
    return await sendSupportMessage(
      adminId,
      `Тикет #${ticket.ticket_number} уже закрыт.`,
    );
  }
  const { error } = await supabase.from("axiom_support_admin_sessions").upsert({
    admin_telegram_user_id: adminId,
    active_ticket_id: ticket.id,
    updated_at: new Date().toISOString(),
  });
  if (error) throw error;
  const { data: history, error: historyError } = await supabase
    .from("axiom_support_messages").select(
      "sender_role,body,content_type,created_at",
    )
    .eq("ticket_id", ticket.id).order("created_at", { ascending: false }).limit(
      12,
    );
  if (historyError) throw historyError;
  const lines = [...(history ?? [])].reverse().map((item) => {
    const author = item.sender_role === "admin" ? "Вы" : "Пользователь";
    const content = item.body ||
      (item.content_type === "photo" ? "[фото]" : "[файл]");
    return `<b>${author}:</b> ${escapeHtml(content)}`;
  });
  await sendSupportMessage(
    adminId,
    `<b>💬 Диалог по тикету #${ticket.ticket_number}</b>\n${
      supportUserLabel(user)
    }\n\n${
      lines.length ? lines.join("\n") : "История пока пуста."
    }\n\n<b>Теперь просто отправляйте сообщения в этот бот — они уйдут пользователю.</b>`,
    ticketKeyboard(ticket.id),
  );
}

async function activeAdminTicket(
  supabase: SupabaseClient,
  adminId: number,
): Promise<SupportTicket | null> {
  const { data: session, error } = await supabase
    .from("axiom_support_admin_sessions").select("active_ticket_id")
    .eq("admin_telegram_user_id", adminId).maybeSingle();
  if (error) throw error;
  if (!session) return null;
  const { data: ticket, error: ticketError } = await supabase
    .from("axiom_support_tickets").select("id,ticket_number,user_id,status")
    .eq("id", session.active_ticket_id).neq("status", "closed").maybeSingle();
  if (ticketError) throw ticketError;
  return ticket as SupportTicket | null;
}

async function listAdminTickets(
  supabase: SupabaseClient,
  adminId: number,
): Promise<void> {
  const { data, error } = await supabase.from("axiom_support_tickets")
    .select("id,ticket_number,user_id,status").neq("status", "closed")
    .order("last_message_at", { ascending: false, nullsFirst: false }).limit(
      20,
    );
  if (error) throw error;
  if (!data?.length) {
    return await sendSupportMessage(adminId, "📭 Открытых тикетов нет.");
  }
  for (const item of data as SupportTicket[]) {
    const { user } = await ticketAndUser(supabase, item.id);
    const status = item.status === "waiting_admin"
      ? "🔴 ждёт ответа"
      : "🟢 ждём пользователя";
    await sendSupportMessage(
      adminId,
      `<b>🎫 Тикет #${item.ticket_number}</b> · ${status}\n${
        supportUserLabel(user)
      }`,
      ticketKeyboard(item.id),
    );
  }
}

async function closeAdminTicket(
  supabase: SupabaseClient,
  adminId: number,
  ticketId: string,
): Promise<void> {
  const { ticket, user } = await ticketAndUser(supabase, ticketId);
  const { error } = await supabase.rpc("axiom_support_close_ticket", {
    p_ticket_id: ticket.id,
    p_closed_by_telegram_user_id: adminId,
  });
  if (error) throw error;
  const locale = normalizeLocale(String(user.language_code ?? "ru"));
  await sendUserMessage(
    Number(user.telegram_user_id),
    tr(
      locale,
      `✅ Оператор закрыл обращение #${ticket.ticket_number}. Если вопрос остался, создайте новый тикет через /support.`,
      `✅ The operator closed ticket #${ticket.ticket_number}. If you still need help, create a new ticket with /support.`,
    ),
    locale,
    "support",
  ).catch((notifyError) =>
    console.error("Could not notify ticket user", notifyError)
  );
  await sendSupportMessage(
    adminId,
    `✅ Тикет #${ticket.ticket_number} закрыт.`,
  );
}

async function handleAdminReply(
  supabase: SupabaseClient,
  adminId: number,
  message: TelegramMessage,
): Promise<void> {
  const ticket = await activeAdminTicket(supabase, adminId);
  if (!ticket) {
    return await sendSupportMessage(
      adminId,
      "Сначала откройте тикет кнопкой «💬 Открыть диалог» или командой /tickets.",
    );
  }
  const content = supportContent(message);
  if (!content) return;
  const storedId = await storeSupportMessage(
    supabase,
    ticket.id,
    "admin",
    "support",
    message,
    content,
  );
  if (!storedId) return;
  const { user } = await ticketAndUser(supabase, ticket.id);
  const userChatId = Number(user.telegram_user_id);
  const locale = normalizeLocale(String(user.language_code ?? "ru"));
  try {
    await sendUserMessage(
      userChatId,
      `${
        tr(
          locale,
          `<b>🛟 Ответ поддержки · тикет #${ticket.ticket_number}</b>`,
          `<b>🛟 Support reply · ticket #${ticket.ticket_number}</b>`,
        )
      }\n\n${
        content.body
          ? escapeHtml(content.body)
          : tr(locale, "📎 Вложение ниже", "📎 Attachment below")
      }`,
      locale,
      "support",
      [[{
        text: tr(locale, "✅ Завершить обращение", "✅ End conversation"),
        callback_data: "support:close",
      }]],
    );
    await relaySupportMedia(
      requiredSecret("SUPPORT_BOT_TOKEN"),
      requiredSecret("TELEGRAM_BOT_TOKEN"),
      userChatId,
      content,
    );
    await markSupportDelivery(supabase, storedId);
    await sendSupportMessage(
      adminId,
      `✅ Ответ доставлен пользователю по тикету #${ticket.ticket_number}.`,
      ticketKeyboard(ticket.id),
    );
  } catch (error) {
    await markSupportDelivery(supabase, storedId, error);
    await sendSupportMessage(
      adminId,
      `⚠️ Не удалось доставить ответ по тикету #${ticket.ticket_number}. Возможно, пользователь заблокировал Axiom-бота.`,
      ticketKeyboard(ticket.id),
    );
  }
}

async function showTrialWarning(chatId: number, locale: Locale): Promise<void> {
  await sendUserMessage(
    chatId,
    tr(
      locale,
      "<b>⚠️ Перед активацией Trial</b>\n\nПробный период можно активировать один раз. Он действует <b>3 дня</b>, а отсчёт начнётся сразу после подтверждения.\n\nПеред продолжением рекомендуем скачать и подготовить лаунчер.",
      "<b>⚠️ Before activating the Trial</b>\n\nThe trial can be activated once. It lasts for <b>3 days</b>, starting immediately after confirmation.\n\nWe recommend downloading and preparing the launcher first.",
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
  if (data.unavailable) {
    return await sendUserMessage(
      chatId,
      tr(
        locale,
        "Trial доступен только до получения первой подписки. У вас уже есть постоянный ключ Axiom; новые покупки будут продлевать его срок.",
        "The trial is only available before the first subscription. You already have a permanent Axiom key; new purchases will extend it.",
      ),
      locale,
      "trial",
    );
  }
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

function activationCodeFromMessage(text: string | undefined): string | null {
  if (!text) return null;
  const match = text.trim().match(/^\/activate(?:@[A-Za-z0-9_]+)?\s+(.+)$/i);
  return match ? normalizeActivationCode(match[1]) : null;
}

async function redeemActivationCode(
  supabase: SupabaseClient,
  user: TelegramUser,
  chatId: number,
  locale: Locale,
  code: string,
): Promise<void> {
  const generatedKey = generateLicenseKey();
  const encryptionKey = requiredSecret("BOT_KEY_ENCRYPTION_KEY");
  const { data, error } = await supabase.rpc("axiom_redeem_activation_code", {
    p_telegram_user_id: user.id,
    p_code_hash: await digestActivationCode(
      code,
      requiredSecret("ACTIVATION_CODE_PEPPER"),
    ),
    p_key_hash: await digestLicense(
      generatedKey,
      requiredSecret("LICENSE_PEPPER"),
    ),
    p_key_ciphertext: await encryptLicense(generatedKey, encryptionKey),
  });
  if (error) throw error;
  const result = data as ActivationFulfillment;
  if (!result.accepted || !result.license || !result.activation) {
    return await sendUserMessage(
      chatId,
      tr(
        locale,
        "Код недействителен или уже был использован другим аккаунтом. Проверьте код в заказе FunPay либо обратитесь в поддержку.",
        "The code is invalid or was already used by another account. Check your FunPay order or contact support.",
      ),
      locale,
      "activate",
    );
  }
  const license = result.license;
  const activation = result.activation;
  const key = await decryptLicense(
    String(license.key_ciphertext),
    encryptionKey,
  );
  const { data: plan, error: planError } = await supabase.from("axiom_plans")
    .select("title,amount_minor,currency").eq("code", activation.plan_code)
    .single();
  if (planError) throw planError;

  await sendUserMessage(
    chatId,
    `<b>${
      result.created_license
        ? tr(locale, "✅ Подписка активирована", "✅ Subscription activated")
        : tr(locale, "✅ Подписка продлена", "✅ Subscription extended")
    }</b>\n\n${tr(locale, "Ключ Axiom", "Axiom key")}: <code>${
      escapeHtml(key)
    }</code>\n${tr(locale, "Действует до", "Valid until")}: ${
      escapeHtml(formatDate(license.expires_at, locale))
    }${
      result.created_license ? "" : tr(
        locale,
        "\n\nКлюч не изменился — к нему добавлено оплаченное время.",
        "\n\nThe key has not changed; the purchased time was added to it.",
      )
    }`,
    locale,
    "keys",
  );

  if (!activation.admin_notified_at) {
    const username = user.username ? `@${escapeHtml(user.username)}` : "—";
    await sendMessage(
      requiredSecret("TELEGRAM_ADMIN_CHAT_ID"),
      `<b>Новая активация FunPay</b>\nTelegram: <code>${user.id}</code> (${username})\nТариф: ${
        escapeHtml(plan.title)
      } (${escapeHtml(activation.plan_code)})\nСумма: ${
        escapeHtml(
          formatMoney(Number(plan.amount_minor), String(plan.currency), "ru"),
        )
      }\nActivation ID: <code>${
        escapeHtml(activation.id)
      }</code>\nКлюч: <code>${escapeHtml(key)}</code>\nДействует до: ${
        escapeHtml(formatDate(license.expires_at, "ru"))
      }`,
    );
    await supabase.from("axiom_activation_codes").update({
      admin_notified_at: new Date().toISOString(),
    }).eq("id", activation.id).is("admin_notified_at", null);
  }
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
  const isTelegramStars = provider.name === "telegram_stars";
  const rpcName = isTelegramStars
    ? "axiom_create_star_order"
    : "axiom_create_order";
  const rpcArgs = isTelegramStars
    ? {
      p_telegram_user_id: user.id,
      p_plan_code: planCode,
      p_telegram_update_id: updateId,
    }
    : {
      p_telegram_user_id: user.id,
      p_plan_code: planCode,
      p_provider: provider.name,
      p_telegram_update_id: updateId,
    };
  const { data: order, error } = await supabase.rpc(rpcName, rpcArgs);
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
      isTelegramStars
        ? `<b>💫 ${
          tr(locale, "Оплата подписки", "Subscription payment")
        }</b>\n\n${tr(locale, "Тариф", "Plan")}: ${
          escapeHtml(planTitle(plan, locale))
        }\n${tr(locale, "Цена", "Price")}: ${
          escapeHtml(formatMoney(plan.amount_minor, plan.currency, locale))
        } = <b>${escapeHtml(order.amount_minor)} ⭐</b>\n\n${
          tr(
            locale,
            "1. Нажмите «Купить звёзды» и пополните баланс.\n2. Вернитесь сюда и нажмите «Потратить звёзды».\n3. Ключ будет выдан автоматически после подтверждения Telegram.",
            "1. Tap “Buy Stars” and top up your balance.\n2. Return here and tap “Spend Stars”.\n3. The key will be issued automatically after Telegram confirms payment.",
          )
        }\n\nOrder ID: <code>${escapeHtml(order.id)}</code>`
        : `<b>${tr(locale, "Заказ", "Order")} ${escapeHtml(order.id)}</b>\n${
          escapeHtml(planTitle(plan, locale))
        } — ${
          escapeHtml(formatMoney(order.amount_minor, order.currency, locale))
        }`,
      locale,
      "buy",
      isTelegramStars
        ? [
          [{
            text: tr(locale, "💫 Купить звёзды", "💫 Buy Stars"),
            url: "https://t.me/starfallrobot",
          }],
          [{
            text: tr(locale, "⭐ Потратить звёзды", "⭐ Spend Stars"),
            url: checkout.url,
          }],
          [{
            text: tr(locale, "🛡 Официальное пополнение", "🛡 Official top-up"),
            url: "https://t.me/PremiumBot",
          }],
        ]
        : [[{ text: tr(locale, "💳 Оплатить", "💳 Pay"), url: checkout.url }]],
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
  const paymentAmount = formatPaymentAmount(
    Number(order.amount_minor),
    String(order.currency),
  );
  const rubPrice = order.price_rub_minor
    ? ` (${formatMoney(Number(order.price_rub_minor), "RUB", "ru")})`
    : "";
  if (!order.admin_notified_at) {
    await sendMessage(
      requiredSecret("TELEGRAM_ADMIN_CHAT_ID"),
      `<b>Новая покупка Axiom</b>\nTelegram: <code>${user.telegram_user_id}</code> (${username})\nТариф: ${
        escapeHtml(plan.title)
      } (${escapeHtml(order.plan_code)})\nСумма: ${
        escapeHtml(`${paymentAmount}${rubPrice}`)
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

async function telegramOrderMatches(
  supabase: SupabaseClient,
  orderId: string,
  telegramUserId: number,
  amount: number,
  currency: string,
  allowFulfilled = false,
): Promise<boolean> {
  const { data: user, error: userError } = await supabase.from("axiom_users")
    .select("id").eq("telegram_user_id", telegramUserId).maybeSingle();
  if (userError || !user) return false;
  const { data: order, error } = await supabase.from("axiom_orders")
    .select("user_id,amount_minor,currency,provider,status").eq("id", orderId)
    .maybeSingle();
  if (error || !order) return false;
  const validStatus = order.status === "pending_payment" ||
    (allowFulfilled && order.status === "fulfilled");
  return order.user_id === user.id && order.provider === "telegram_stars" &&
    validStatus && order.currency === currency &&
    Number(order.amount_minor) === amount;
}

async function handlePreCheckout(
  supabase: SupabaseClient,
  query: NonNullable<TelegramUpdate["pre_checkout_query"]>,
): Promise<void> {
  const orderId = orderIdFromTelegramPayload(query.invoice_payload);
  const ok = orderId !== null && query.currency === "XTR" &&
    Number.isSafeInteger(query.total_amount) && query.total_amount > 0 &&
    await telegramOrderMatches(
      supabase,
      orderId,
      query.from.id,
      query.total_amount,
      query.currency,
    );
  await telegramCall("answerPreCheckoutQuery", {
    pre_checkout_query_id: query.id,
    ok,
    ...(ok ? {} : {
      error_message:
        "Заказ недействителен или его цена изменилась. Вернитесь в бот и создайте новый заказ.",
    }),
  });
}

async function handleSuccessfulTelegramPayment(
  supabase: SupabaseClient,
  update: TelegramUpdate,
  message: TelegramMessage,
): Promise<void> {
  const payment = message.successful_payment!;
  const user = message.from!;
  const orderId = orderIdFromTelegramPayload(payment.invoice_payload);
  if (
    !orderId || payment.currency !== "XTR" ||
    !await telegramOrderMatches(
      supabase,
      orderId,
      user.id,
      payment.total_amount,
      payment.currency,
      true,
    )
  ) {
    throw new Error("Telegram successful_payment does not match its order");
  }
  const fulfillment = await fulfillPayment(supabase, {
    provider: "telegram_stars",
    eventId: `telegram-update:${update.update_id}`,
    externalPaymentId: payment.telegram_payment_charge_id,
    orderId,
    amountMinor: payment.total_amount,
    currency: payment.currency,
  });
  if (fulfillment.accepted) await notifyPurchase(supabase, fulfillment);
}

async function handleSupportTelegram(
  request: Request,
  supabase: SupabaseClient,
): Promise<Response> {
  if (!supportConfigured()) {
    return new Response("not configured", { status: 503 });
  }
  if (
    request.headers.get("x-telegram-bot-api-secret-token") !==
      requiredSecret("SUPPORT_WEBHOOK_SECRET")
  ) {
    return new Response("unauthorized", { status: 401 });
  }
  const update = await request.json() as TelegramUpdate;
  const callback = update.callback_query;
  const message = update.message;
  const user = callback?.from ?? message?.from;
  const chatId = callback?.message?.chat.id ?? message?.chat.id;
  if (!user || !chatId) return new Response("ok");
  if (!supportAdminIds().includes(user.id) || chatId !== user.id) {
    await sendSupportMessage(chatId, "⛔ Доступ запрещён.").catch(() =>
      undefined
    );
    return new Response("ok");
  }
  if (callback) {
    await supportTelegramCall("answerCallbackQuery", {
      callback_query_id: callback.id,
    });
    const openId = ticketIdFromCallback(callback.data, "open");
    const closeId = ticketIdFromCallback(callback.data, "close");
    if (openId) await openAdminTicket(supabase, user.id, openId);
    else if (closeId) await closeAdminTicket(supabase, user.id, closeId);
    return new Response("ok");
  }
  if (!message) return new Response("ok");
  const command = message.text?.trim().split(/\s+/, 1)[0]?.toLowerCase();
  if (command === "/tickets" || command === "/start") {
    if (command === "/start") {
      await sendSupportMessage(
        chatId,
        "<b>🛟 Axiom Support Desk</b>\n\nЗдесь приходят обращения пользователей. Откройте тикет кнопкой и затем пишите ответы обычными сообщениями.",
      );
    }
    await listAdminTickets(supabase, user.id);
  } else if (command === "/cancel") {
    const { error } = await supabase.from("axiom_support_admin_sessions")
      .delete().eq("admin_telegram_user_id", user.id);
    if (error) throw error;
    await sendSupportMessage(
      chatId,
      "⏸ Диалог снят с выбора. Тикет остался открытым.",
    );
  } else if (command === "/close") {
    const ticket = await activeAdminTicket(supabase, user.id);
    if (ticket) await closeAdminTicket(supabase, user.id, ticket.id);
    else await sendSupportMessage(chatId, "Сначала откройте тикет.");
  } else await handleAdminReply(supabase, user.id, message);
  return new Response("ok");
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
  if (update.pre_checkout_query) {
    await handlePreCheckout(supabase, update.pre_checkout_query);
    return new Response("ok");
  }
  const callback = update.callback_query;
  const message = update.message;
  const user = callback?.from ?? message?.from;
  const chatId = callback?.message?.chat.id ?? message?.chat.id;
  if (!user || !chatId) return new Response("ok");
  await upsertUser(supabase, user);
  if (message?.successful_payment) {
    await handleSuccessfulTelegramPayment(supabase, update, message);
    return new Response("ok");
  }
  if (callback) {
    await telegramCall("answerCallbackQuery", {
      callback_query_id: callback.id,
    });
  }

  let locale = await loadLocale(supabase, user.id);
  const callbackAction = callback?.data;
  const command = commandFromText(message?.text);

  if (
    message && !callback && !command &&
    await handleUserSupportMessage(supabase, user, message, locale)
  ) return new Response("ok");

  if (callbackAction === "support:close") {
    await closeUserSupportTicket(supabase, user, chatId, locale);
  } else if (callbackAction?.startsWith("lang:")) {
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
  } else if (
    callbackAction?.startsWith("offer:") || callbackAction?.startsWith("buy:")
  ) {
    await showFunPayOffer(
      supabase,
      chatId,
      locale,
      callbackAction.split(":", 2)[1],
    );
  } else {
    const action = callbackAction?.startsWith("nav:")
      ? callbackAction.slice(4)
      : callbackAction === "licenses"
      ? "keys"
      : callbackAction ?? command ?? "menu";
    if (action === "download") await showDownload(chatId, locale);
    else if (action === "buy") await showBuy(supabase, chatId, locale);
    else if (action === "activate") {
      const code = activationCodeFromMessage(message?.text);
      if (code) {
        await redeemActivationCode(supabase, user, chatId, locale, code);
      } else await showActivate(chatId, locale);
    } else if (action === "language") await showLanguage(chatId, locale);
    else if (action === "keys") {
      await showLicenses(supabase, user, chatId, locale);
    } else if (action === "trial:confirm") {
      await issueTrial(supabase, user, chatId, locale);
    } else if (action === "trial") {
      await showTrialWarning(chatId, locale);
    } else if (action === "guide") await showGuide(chatId, locale);
    else if (action === "support") {
      await showSupport(supabase, user, chatId, locale);
    } else await showMenu(chatId, locale);
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
        telegram_webhook_configured: true,
        support_bot_configured: supportConfigured(),
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
    if (url.pathname.endsWith("/support-telegram")) {
      return await handleSupportTelegram(request, supabase);
    }
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
