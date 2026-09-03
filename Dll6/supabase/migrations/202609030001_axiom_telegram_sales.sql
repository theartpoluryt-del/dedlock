-- Telegram sales layer for the existing Axiom licensing backend.
-- All access is service-role only; public clients never receive table grants.

create table if not exists public.axiom_users (
    id uuid primary key default gen_random_uuid(),
    telegram_user_id bigint not null unique check (telegram_user_id > 0),
    username text check (username is null or length(username) <= 64),
    first_name text check (first_name is null or length(first_name) <= 128),
    last_name text check (last_name is null or length(last_name) <= 128),
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

create table if not exists public.axiom_plans (
    code text primary key check (code ~ '^[a-z0-9_-]{2,32}$'),
    title text not null check (length(title) between 1 and 100),
    duration_days integer not null check (duration_days between 1 and 3650),
    amount_minor integer not null check (amount_minor > 0),
    currency text not null check (currency ~ '^[A-Z]{3}$'),
    active boolean not null default true,
    sort_order integer not null default 0,
    created_at timestamptz not null default now()
);

insert into public.axiom_plans(code, title, duration_days, amount_minor, currency, sort_order)
values
    ('month', '30 дней', 30, 99000, 'RUB', 10),
    ('quarter', '90 дней', 90, 249000, 'RUB', 20),
    ('year', '365 дней', 365, 799000, 'RUB', 30)
on conflict (code) do nothing;

create type public.axiom_order_status as enum (
    'pending_payment', 'paid', 'fulfilling', 'fulfilled',
    'canceled', 'expired', 'failed', 'refunded'
);

create type public.axiom_payment_status as enum (
    'received', 'succeeded', 'rejected', 'refunded'
);

create table if not exists public.axiom_orders (
    id uuid primary key default gen_random_uuid(),
    user_id uuid not null references public.axiom_users(id),
    plan_code text not null references public.axiom_plans(code),
    duration_days integer not null check (duration_days between 1 and 3650),
    amount_minor integer not null check (amount_minor > 0),
    currency text not null check (currency ~ '^[A-Z]{3}$'),
    status public.axiom_order_status not null default 'pending_payment',
    provider text not null check (provider ~ '^[a-z0-9_-]{2,32}$'),
    telegram_update_id bigint unique,
    provider_checkout_id text,
    license_id uuid references public.axiom_licenses(id),
    failure_reason text,
    created_at timestamptz not null default now(),
    paid_at timestamptz,
    fulfilled_at timestamptz,
    buyer_notified_at timestamptz,
    admin_notified_at timestamptz,
    updated_at timestamptz not null default now()
);

create index if not exists axiom_orders_user_created_idx
    on public.axiom_orders(user_id, created_at desc);
create index if not exists axiom_orders_status_created_idx
    on public.axiom_orders(status, created_at);

create table if not exists public.axiom_payments (
    id uuid primary key default gen_random_uuid(),
    order_id uuid not null references public.axiom_orders(id),
    provider text not null check (provider ~ '^[a-z0-9_-]{2,32}$'),
    provider_event_id text not null,
    external_payment_id text not null,
    amount_minor integer not null check (amount_minor >= 0),
    currency text not null check (currency ~ '^[A-Z]{3}$'),
    status public.axiom_payment_status not null default 'received',
    failure_reason text,
    received_at timestamptz not null default now(),
    processed_at timestamptz,
    unique(provider, provider_event_id),
    unique(provider, external_payment_id)
);

create unique index if not exists axiom_one_succeeded_payment_per_order_idx
    on public.axiom_payments(order_id) where status = 'succeeded';

create table if not exists public.axiom_trials (
    user_id uuid primary key references public.axiom_users(id),
    license_id uuid not null unique references public.axiom_licenses(id),
    claimed_at timestamptz not null default now(),
    expires_at timestamptz not null,
    check (expires_at > claimed_at)
);

-- The plaintext license is encrypted by the Edge Function with AES-GCM.  The
-- launcher still uses only key_hash; ciphertext exists solely for reliable
-- Telegram redelivery after retries or webhook timeouts.
alter table public.axiom_licenses
    add column if not exists user_id uuid references public.axiom_users(id),
    add column if not exists key_ciphertext text,
    add column if not exists source_kind text check (source_kind in ('admin', 'trial', 'order')),
    add column if not exists source_id uuid;

create unique index if not exists axiom_licenses_source_idx
    on public.axiom_licenses(source_kind, source_id)
    where source_kind in ('trial', 'order');
create index if not exists axiom_licenses_user_created_idx
    on public.axiom_licenses(user_id, created_at desc);

alter table public.axiom_users enable row level security;
alter table public.axiom_plans enable row level security;
alter table public.axiom_orders enable row level security;
alter table public.axiom_payments enable row level security;
alter table public.axiom_trials enable row level security;

revoke all on public.axiom_users, public.axiom_plans, public.axiom_orders,
    public.axiom_payments, public.axiom_trials from public, anon, authenticated;
grant select, insert, update, delete on public.axiom_users, public.axiom_plans,
    public.axiom_orders, public.axiom_payments, public.axiom_trials to service_role;

create or replace function public.axiom_upsert_telegram_user(
    p_telegram_user_id bigint,
    p_username text default null,
    p_first_name text default null,
    p_last_name text default null
) returns uuid
language plpgsql security definer set search_path = public, pg_temp as $$
declare v_user_id uuid;
begin
    if p_telegram_user_id <= 0 then raise exception 'invalid Telegram user id'; end if;
    insert into public.axiom_users(telegram_user_id, username, first_name, last_name)
    values (p_telegram_user_id, nullif(left(p_username, 64), ''),
            nullif(left(p_first_name, 128), ''), nullif(left(p_last_name, 128), ''))
    on conflict (telegram_user_id) do update set
        username = excluded.username,
        first_name = excluded.first_name,
        last_name = excluded.last_name,
        updated_at = now()
    returning id into v_user_id;
    return v_user_id;
end;
$$;

create or replace function public.axiom_claim_trial(
    p_telegram_user_id bigint,
    p_key_hash text,
    p_key_ciphertext text
) returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
    v_user public.axiom_users%rowtype;
    v_trial public.axiom_trials%rowtype;
    v_license_id uuid;
    v_expires_at timestamptz := now() + interval '3 days';
begin
    if p_key_hash !~ '^[0-9a-f]{64}$' or length(p_key_ciphertext) < 24 then
        raise exception 'invalid license material';
    end if;
    perform pg_advisory_xact_lock(p_telegram_user_id);
    select * into v_user from public.axiom_users
      where telegram_user_id = p_telegram_user_id for update;
    if not found then raise exception 'Telegram user not registered'; end if;

    select * into v_trial from public.axiom_trials where user_id = v_user.id;
    if found then
        return (select jsonb_build_object(
            'created', false, 'license_id', l.id, 'key_ciphertext', l.key_ciphertext,
            'expires_at', v_trial.expires_at)
            from public.axiom_licenses l where l.id = v_trial.license_id);
    end if;

    insert into public.axiom_licenses(
        key_hash, label, expires_at, max_devices, user_id, key_ciphertext,
        source_kind, source_id
    ) values (
        p_key_hash, 'Telegram trial ' || p_telegram_user_id, v_expires_at, 1,
        v_user.id, p_key_ciphertext, 'trial', v_user.id
    ) returning id into v_license_id;
    insert into public.axiom_trials(user_id, license_id, expires_at)
    values (v_user.id, v_license_id, v_expires_at);
    return jsonb_build_object(
        'created', true, 'license_id', v_license_id,
        'key_ciphertext', p_key_ciphertext, 'expires_at', v_expires_at);
end;
$$;

create or replace function public.axiom_create_order(
    p_telegram_user_id bigint,
    p_plan_code text,
    p_provider text,
    p_telegram_update_id bigint
) returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare v_user_id uuid; v_plan public.axiom_plans%rowtype; v_order public.axiom_orders%rowtype;
begin
    select * into v_order from public.axiom_orders
      where telegram_update_id = p_telegram_update_id;
    if found then return to_jsonb(v_order); end if;
    select id into v_user_id from public.axiom_users
      where telegram_user_id = p_telegram_user_id;
    if v_user_id is null then raise exception 'Telegram user not registered'; end if;
    select * into v_plan from public.axiom_plans where code = p_plan_code and active;
    if not found then raise exception 'plan unavailable'; end if;
    if p_provider !~ '^[a-z0-9_-]{2,32}$' then raise exception 'invalid provider'; end if;
    insert into public.axiom_orders(
        user_id, plan_code, duration_days, amount_minor, currency, provider,
        telegram_update_id
    ) values (
        v_user_id, v_plan.code, v_plan.duration_days, v_plan.amount_minor,
        v_plan.currency, p_provider, p_telegram_update_id
    ) returning * into v_order;
    return to_jsonb(v_order);
end;
$$;

create or replace function public.axiom_fulfill_paid_order(
    p_order_id uuid,
    p_provider text,
    p_provider_event_id text,
    p_external_payment_id text,
    p_amount_minor integer,
    p_currency text,
    p_key_hash text,
    p_key_ciphertext text
) returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
    v_order public.axiom_orders%rowtype;
    v_payment public.axiom_payments%rowtype;
    v_license public.axiom_licenses%rowtype;
    v_expires_at timestamptz;
begin
    if p_key_hash !~ '^[0-9a-f]{64}$' or length(p_key_ciphertext) < 24
       or p_provider_event_id = '' or p_external_payment_id = '' then
        raise exception 'invalid fulfillment material';
    end if;
    perform pg_advisory_xact_lock(hashtextextended(p_order_id::text, 0));

    select * into v_payment from public.axiom_payments
      where provider = p_provider
        and (provider_event_id = p_provider_event_id
             or external_payment_id = p_external_payment_id);
    if found then
        if v_payment.order_id <> p_order_id then
            raise exception 'payment identifier belongs to another order';
        end if;
        select * into v_order from public.axiom_orders where id = v_payment.order_id;
        select * into v_license from public.axiom_licenses where id = v_order.license_id;
        return jsonb_build_object(
            'accepted', v_payment.status = 'succeeded',
            'idempotent', true, 'reason', v_payment.failure_reason,
            'order', to_jsonb(v_order), 'license', to_jsonb(v_license));
    end if;

    select * into v_order from public.axiom_orders where id = p_order_id for update;
    if not found or v_order.provider <> p_provider then raise exception 'order not found'; end if;
    if v_order.status = 'fulfilled' then
        raise exception 'order already fulfilled by another payment';
    end if;
    if v_order.status <> 'pending_payment' then raise exception 'order is not payable'; end if;

    if p_amount_minor <> v_order.amount_minor or upper(p_currency) <> v_order.currency then
        insert into public.axiom_payments(
            order_id, provider, provider_event_id, external_payment_id,
            amount_minor, currency, status, failure_reason, processed_at
        ) values (
            v_order.id, p_provider, p_provider_event_id, p_external_payment_id,
            greatest(p_amount_minor, 0), upper(p_currency), 'rejected',
            'amount_or_currency_mismatch', now()
        );
        return jsonb_build_object('accepted', false, 'reason', 'amount_or_currency_mismatch');
    end if;

    insert into public.axiom_payments(
        order_id, provider, provider_event_id, external_payment_id,
        amount_minor, currency, status, processed_at
    ) values (
        v_order.id, p_provider, p_provider_event_id, p_external_payment_id,
        p_amount_minor, upper(p_currency), 'succeeded', now()
    ) returning * into v_payment;

    v_expires_at := now() + make_interval(days => v_order.duration_days);
    insert into public.axiom_licenses(
        key_hash, label, expires_at, max_devices, user_id, key_ciphertext,
        source_kind, source_id
    ) values (
        p_key_hash, 'Telegram order ' || v_order.id, v_expires_at, 1,
        v_order.user_id, p_key_ciphertext, 'order', v_order.id
    ) returning * into v_license;

    update public.axiom_orders set
        status = 'fulfilled', license_id = v_license.id,
        paid_at = now(), fulfilled_at = now(), updated_at = now()
      where id = v_order.id returning * into v_order;
    return jsonb_build_object('accepted', true, 'idempotent', false,
                              'order', to_jsonb(v_order), 'license', to_jsonb(v_license));
end;
$$;

revoke all on function public.axiom_upsert_telegram_user(bigint,text,text,text),
    public.axiom_claim_trial(bigint,text,text),
    public.axiom_create_order(bigint,text,text,bigint),
    public.axiom_fulfill_paid_order(uuid,text,text,text,integer,text,text,text)
    from public, anon, authenticated;
grant execute on function public.axiom_upsert_telegram_user(bigint,text,text,text),
    public.axiom_claim_trial(bigint,text,text),
    public.axiom_create_order(bigint,text,text,bigint),
    public.axiom_fulfill_paid_order(uuid,text,text,text,integer,text,text,text)
    to service_role;
