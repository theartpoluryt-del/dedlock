-- One-time activation vouchers intended for FunPay's built-in auto-delivery.
-- Only HMAC digests are stored; plaintext vouchers exist only in seller stock.

alter table public.axiom_plans
    add column if not exists purchase_url text
        check (purchase_url is null or purchase_url ~ '^https://funpay[.]com/');

create table if not exists public.axiom_activation_codes (
    id uuid primary key default gen_random_uuid(),
    code_hash text not null unique check (code_hash ~ '^[0-9a-f]{64}$'),
    plan_code text not null references public.axiom_plans(code),
    batch_id uuid not null,
    status text not null default 'available'
        check (status in ('available', 'redeemed', 'revoked')),
    redeemed_by_user_id uuid references public.axiom_users(id),
    license_id uuid unique references public.axiom_licenses(id),
    created_at timestamptz not null default now(),
    redeemed_at timestamptz,
    admin_notified_at timestamptz,
    check (
        (status = 'redeemed' and redeemed_by_user_id is not null
         and license_id is not null and redeemed_at is not null)
        or
        (status <> 'redeemed' and redeemed_by_user_id is null
         and license_id is null and redeemed_at is null)
    )
);

create index if not exists axiom_activation_codes_stock_idx
    on public.axiom_activation_codes(plan_code, status, created_at);
create index if not exists axiom_activation_codes_user_idx
    on public.axiom_activation_codes(redeemed_by_user_id, redeemed_at desc)
    where redeemed_by_user_id is not null;

alter table public.axiom_activation_codes enable row level security;
revoke all on public.axiom_activation_codes from public, anon, authenticated;
grant select, insert, update, delete on public.axiom_activation_codes to service_role;

alter table public.axiom_licenses
    drop constraint if exists axiom_licenses_source_kind_check;
alter table public.axiom_licenses
    add constraint axiom_licenses_source_kind_check
    check (source_kind in ('admin', 'trial', 'order', 'activation'));

drop index if exists public.axiom_licenses_source_idx;
create unique index axiom_licenses_source_idx
    on public.axiom_licenses(source_kind, source_id)
    where source_kind in ('trial', 'order', 'activation');

create or replace function public.axiom_redeem_activation_code(
    p_telegram_user_id bigint,
    p_code_hash text,
    p_key_hash text,
    p_key_ciphertext text
) returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
    v_user public.axiom_users%rowtype;
    v_code public.axiom_activation_codes%rowtype;
    v_plan public.axiom_plans%rowtype;
    v_license public.axiom_licenses%rowtype;
    v_expires_at timestamptz;
begin
    if p_telegram_user_id <= 0 or p_code_hash !~ '^[0-9a-f]{64}$'
       or p_key_hash !~ '^[0-9a-f]{64}$' or length(p_key_ciphertext) < 24 then
        raise exception 'invalid activation material';
    end if;
    perform pg_advisory_xact_lock(hashtextextended(p_code_hash, 0));

    select * into v_user from public.axiom_users
      where telegram_user_id = p_telegram_user_id for update;
    if not found then raise exception 'Telegram user not registered'; end if;

    select * into v_code from public.axiom_activation_codes
      where code_hash = p_code_hash for update;
    if not found or v_code.status = 'revoked' then
        return jsonb_build_object('accepted', false, 'reason', 'invalid_code');
    end if;

    if v_code.status = 'redeemed' then
        if v_code.redeemed_by_user_id <> v_user.id then
            return jsonb_build_object('accepted', false, 'reason', 'already_redeemed');
        end if;
        select * into v_license from public.axiom_licenses
          where id = v_code.license_id;
        return jsonb_build_object(
            'accepted', true, 'idempotent', true,
            'activation', to_jsonb(v_code), 'license', to_jsonb(v_license));
    end if;

    select * into v_plan from public.axiom_plans
      where code = v_code.plan_code;
    if not found then raise exception 'activation plan unavailable'; end if;
    v_expires_at := now() + make_interval(days => v_plan.duration_days);

    insert into public.axiom_licenses(
        key_hash, label, expires_at, max_devices, user_id, key_ciphertext,
        source_kind, source_id
    ) values (
        p_key_hash, 'FunPay activation ' || v_code.id, v_expires_at, 1,
        v_user.id, p_key_ciphertext, 'activation', v_code.id
    ) returning * into v_license;

    update public.axiom_activation_codes set
        status = 'redeemed', redeemed_by_user_id = v_user.id,
        license_id = v_license.id, redeemed_at = now()
      where id = v_code.id returning * into v_code;

    return jsonb_build_object(
        'accepted', true, 'idempotent', false,
        'activation', to_jsonb(v_code), 'license', to_jsonb(v_license));
end;
$$;

revoke all on function public.axiom_redeem_activation_code(bigint,text,text,text)
    from public, anon, authenticated;
grant execute on function public.axiom_redeem_activation_code(bigint,text,text,text)
    to service_role;
