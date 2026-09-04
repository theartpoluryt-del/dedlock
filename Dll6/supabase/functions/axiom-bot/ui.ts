export type Locale = "ru" | "en";
export type Section =
  | "menu"
  | "download"
  | "buy"
  | "activate"
  | "language"
  | "keys"
  | "trial"
  | "guide"
  | "support";

export type InlineButton = {
  text: string;
  callback_data?: string;
  url?: string;
};

const labels = {
  ru: {
    download: "⬇️ Скачать",
    buy: "💲 Купить",
    activate: "🔐 Активировать код",
    language: "🌐 Язык",
    keys: "🔑 Мои ключи",
    trial: "🎁 Trial на 3 дня",
    guide: "📖 Инструкция",
    support: "🛟 Поддержка",
    menu: "🏠 Главное меню",
  },
  en: {
    download: "⬇️ Download",
    buy: "💲 Buy",
    activate: "🔐 Activate code",
    language: "🌐 Language",
    keys: "🔑 My keys",
    trial: "🎁 3-day trial",
    guide: "📖 Guide",
    support: "🛟 Support",
    menu: "🏠 Main menu",
  },
} as const;

const callbacks: Record<Section, string> = {
  menu: "nav:menu",
  download: "nav:download",
  buy: "nav:buy",
  activate: "nav:activate",
  language: "nav:language",
  keys: "nav:keys",
  trial: "nav:trial",
  guide: "nav:guide",
  support: "nav:support",
};

const contextSections: Record<Section, Section[][]> = {
  menu: [
    ["download", "buy"],
    ["keys", "trial"],
    ["guide", "support"],
    ["language"],
  ],
  download: [["guide", "menu"]],
  buy: [["keys", "menu"]],
  activate: [["keys", "menu"]],
  language: [["menu"]],
  keys: [["buy", "download"], ["menu"]],
  trial: [["download", "menu"]],
  guide: [["download", "keys"], ["support", "menu"]],
  support: [["guide", "menu"]],
};

export function normalizeLocale(value: unknown): Locale {
  return value === "en" ? "en" : "ru";
}

export function navigationKeyboard(
  locale: Locale,
  section: Section,
): InlineButton[][] {
  return contextSections[section].map((row) =>
    row.map((item) => ({
      text: labels[locale][item],
      callback_data: callbacks[item],
    }))
  );
}

export function commandDefinitions(locale: Locale) {
  const descriptions = locale === "ru"
    ? {
      download: "⬇️ скачать загрузчик",
      buy: "💲 купить подписку",
      activate: "🔐 активировать код FunPay",
      language: "🇺🇸 выбрать язык",
      keys: "🔑 мои ключи",
      trial: "🎁 бесплатный период на 3 дня",
      guide: "📖 инструкция по запуску",
      support: "🛟 связаться с поддержкой",
    }
    : {
      download: "⬇️ download launcher",
      buy: "💲 buy a subscription",
      activate: "🔐 activate a FunPay code",
      language: "🇷🇺 choose language",
      keys: "🔑 my license keys",
      trial: "🎁 free 3-day trial",
      guide: "📖 setup and launch guide",
      support: "🛟 contact support",
    };
  return Object.entries(descriptions).map(([command, description]) => ({
    command,
    description,
  }));
}

export function commandFromText(text: string | undefined): string | null {
  if (!text?.startsWith("/")) return null;
  return text.trim().split(/\s+/, 1)[0].slice(1).split("@", 1)[0].toLowerCase();
}
