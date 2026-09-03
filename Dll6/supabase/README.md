# Axiom licensing on Supabase

The launcher contains only the verification public key and the Edge Function URL.
The database is private (RLS plus revoked client grants). The service-role key,
license pepper, IP hashing pepper, and P-256 signing key are Edge Function secrets.
Modules live in the private `axiom-modules` bucket and are streamed in
authenticated chunks after a successful license/device check. Every delivered
module contains three HMAC-authenticated watermark records bound to its license,
device, and release. The signed session covers the personalized module hash.

## Deploy

1. Install/login to the Supabase CLI and link this directory to a project.
2. Apply `migrations/202609010001_axiom_licensing.sql`.
3. Generate two independent random 32-byte values for `LICENSE_PEPPER` and
   `IP_HASH_PEPPER`. Never change `LICENSE_PEPPER` after issuing keys.
4. Convert the existing compatible P-256 key locally:
   `python tools/export_signing_key.py ../license-server/data/signing-key.pem`.
5. Set all three values with `supabase secrets set`; never commit them.
6. Deploy with `supabase functions deploy axiom-license --no-verify-jwt`.
7. Pin the function URL in `kDefaultApiBase` in `launcher_auth.cpp` before
   distribution. Production builds intentionally ignore writable URL overrides.

The function URL is intentionally public: requests reveal no database credential,
all successful responses are bound to the device and one-time nonce, and the
launcher rejects responses without a valid ECDSA signature.

## Administration

Run `tools/admin.py` only on an administrator machine. Set `SUPABASE_URL`,
`SUPABASE_SERVICE_ROLE_KEY`, and the same `LICENSE_PEPPER` in that terminal.
On Windows, `tools/admin.ps1` can keep these values in a current-user DPAPI blob
under `%LOCALAPPDATA%\Axiom\admin` instead of a plaintext `.env` file.

```text
python tools/admin.py create-license --label customer --max-devices 1
python tools/admin.py publish ..\x64\Release\Dll6.dll --version 1.0.2 --minimum-launcher 1.0.1
python tools/admin.py revoke-license AXM-XXXXX-XXXXX-XXXXX-XXXXX
python tools/admin.py identify-watermark leaked.dll
```

Only the generated plaintext key is shown once; the database stores its HMAC.
`identify-watermark` accepts a delivered or memory-dumped module and reports
only records whose HMAC validates against the current licensing database.

## Telegram subscription bot

`functions/axiom-bot` is the sales bot for `AxiomLauncher.exe`. It reuses
`axiom_licenses` and the same `LICENSE_PEPPER`, so keys issued by the bot work
with the existing launcher and `axiom-license` Edge Function. The sales
migration adds private `axiom_users`, `axiom_plans`, `axiom_orders`,
`axiom_payments`, and `axiom_trials` tables. The existing `axiom_licenses` table
is the single license source of truth.

Security properties:

- all sales tables have RLS enabled, no `anon`/`authenticated` grants, and are
  accessed only by the Edge Function service role;
- the service-role key, Telegram token, license pepper, and AES key stay in
  Supabase Edge Function secrets and never enter the launcher or Git;
- one trial per immutable Telegram user id is enforced transactionally by a
  primary key plus an advisory lock; concurrent/repeated callbacks return the
  original trial rather than creating another license;
- payment webhooks must be cryptographically verified by a provider adapter
  before `axiom_fulfill_paid_order` is called;
- `(provider, provider_event_id)` and `(provider, external_payment_id)` are
  unique, fulfillment locks the order, verifies the stored amount/currency,
  and creates the payment and license in one transaction;
- plaintext keys are never stored directly. An AES-256-GCM envelope is retained
  only so a committed key can be redelivered after Telegram/network retries.

### Configure and deploy

1. Create a bot with BotFather and obtain the numeric owner chat id.
2. Review the seeded prices in
   `migrations/202609030001_axiom_telegram_sales.sql` before applying it.
3. Copy `.env.example` only as a reference. Generate independent secrets, for
   example with `openssl rand -base64 32` for `BOT_KEY_ENCRYPTION_KEY` and
   `openssl rand -hex 32` for `TELEGRAM_WEBHOOK_SECRET`.
4. Link the directory and apply migrations:

   ```powershell
   npx supabase login
   npx supabase link --project-ref vljgmubfztmxsyiwrity
   npx supabase db push
   ```

5. Set secrets without writing them to a tracked file. Use an independent
   random `ACTIVATION_CODE_PEPPER`; changing it invalidates unsold FunPay codes:

   ```powershell
   npx supabase secrets set TELEGRAM_BOT_TOKEN=... TELEGRAM_ADMIN_CHAT_ID=... TELEGRAM_WEBHOOK_SECRET=... BOT_KEY_ENCRYPTION_KEY=... LICENSE_PEPPER=... ACTIVATION_CODE_PEPPER=... DISPLAY_TIME_ZONE=Asia/Yekaterinburg PAYMENT_PROVIDER=disabled
   npx supabase functions deploy axiom-bot --no-verify-jwt
   ```

6. Register the Telegram webhook (replace values locally):

   ```powershell
   curl.exe -X POST "https://api.telegram.org/bot<TOKEN>/setWebhook" -H "Content-Type: application/json" -d '{"url":"https://vljgmubfztmxsyiwrity.supabase.co/functions/v1/axiom-bot/telegram","secret_token":"<TELEGRAM_WEBHOOK_SECRET>","allowed_updates":["message","callback_query","pre_checkout_query"],"drop_pending_updates":true}'
   ```

7. Check `https://vljgmubfztmxsyiwrity.supabase.co/functions/v1/axiom-bot/health`,
   then send `/start` to the bot. Rotate a compromised secret immediately.

The bot supports Russian and English and stores the selected language on the
server. Its Telegram command menu contains `/download`, `/buy`, `/language`,
`/keys`, `/activate`, `/trial`, `/guide`, and `/support`. Every user response includes an
inline keyboard tailored to the current section and a route back to the main
menu. `AXIOM_DOWNLOAD_URL` and `AXIOM_SUPPORT_URL` are optional server-side
configuration values; the defaults use the tracked launcher executable and the
Telegram owner account respectively.

Alternatively, add the following GitHub Actions repository secrets and run
the manual **Deploy Axiom Telegram bot** workflow: `SUPABASE_ACCESS_TOKEN`,
`SUPABASE_DB_PASSWORD`, `TELEGRAM_BOT_TOKEN`, `TELEGRAM_ADMIN_CHAT_ID`,
`TELEGRAM_WEBHOOK_SECRET`, `BOT_KEY_ENCRYPTION_KEY`, `LICENSE_PEPPER`, and
`ACTIVATION_CODE_PEPPER`.
The workflow applies migrations, sets Edge secrets, deploys the function,
registers the Telegram webhook, and checks the health endpoint. It is manual
only, so pushing code cannot unexpectedly mutate production.

### FunPay fulfillment

The bot does not use an unofficial FunPay API, browser cookies, or a seller
userbot. FunPay's native auto-delivery gives the buyer a pre-generated one-time
`AXF-...` code. `/activate CODE` exchanges it for the separate `AXM-...` launcher
key. Redemption uses an advisory lock and one database transaction. A retry by
the same Telegram account returns the original key; another account cannot reuse
the code. Only an HMAC digest of each activation code is stored in Supabase.

Create one FunPay lot for every active plan, enable automatic delivery, then put
the exact offer links in `axiom_plans` (never guess an offer URL):

```sql
update public.axiom_plans set purchase_url = 'https://funpay.com/your-exact-offer-url'
where code = 'three_days';
```

Or set a validated exact offer URL through the DPAPI-backed admin tool:

```powershell
.\tools\admin.ps1 set-plan-purchase-url --plan three_days --url "https://funpay.com/lots/offer?id=12345678"
```

Generate stock only on a trusted administrator machine. The output file contains
bearer credentials: upload it to the matching FunPay lot, verify the stock count,
then store it encrypted or delete it. `.private/` is ignored by Git.

```powershell
python tools/admin.py generate-activation-codes --plan three_days --count 50 --output .private/funpay-three-days.txt
python tools/admin.py generate-activation-codes --plan week --count 50 --output .private/funpay-week.txt
```

The command inserts only digests into Supabase and refuses to overwrite an
existing export. Keep `PAYMENT_PROVIDER=disabled`. Legacy Telegram Stars webhook
handling remains solely so invoices created before the switch can still settle.

### Operations

Orders progress through `pending_payment` to `fulfilled`; terminal alternatives
are `canceled`, `expired`, `failed`, or `refunded`. A successful fulfillment
sends the buyer their key and sends the owner Telegram user id/username, plan,
amount, order id, key, and expiration. Notification timestamps allow webhook
retries to redeliver after a transient Telegram failure.

Run function tests with:

```powershell
npx deno test --config functions/axiom-bot/deno.json functions/axiom-bot/*_test.ts
npx deno check --config functions/axiom-bot/deno.json functions/axiom-bot/index.ts
```
