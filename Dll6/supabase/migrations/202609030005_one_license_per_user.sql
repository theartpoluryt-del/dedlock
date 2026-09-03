-- One Telegram account owns one stable launcher key. Purchases extend the
-- existing key instead of creating replacements.

alter table public.axiom_activation_codes
    drop constraint if exists axiom_activation_codes_license_id_key;

create temporary table axiom_user_license_rollup on commit drop as
select
    user_id,
    (array_agg(id order by enabled desc, (key_ciphertext is not null) desc,
                           expires_at desc nulls first, created_at desc))[1]
        as canonical_id,
    bool_or(enabled) as any_enabled,
    bool_or(enabled and expires_at is null) as has_unlimited,
    coalesce(sum(greatest(extract(epoch from (expires_at - now())), 0))
        filter (where enabled and expires_at is not null), 0) as remaining_seconds
from public.axiom_licenses
where user_id is not null
group by user_id;

update public.axiom_licenses l set
    enabled = r.any_enabled,
    expires_at = case when r.has_unlimited then null
                      else now() + make_interval(secs => r.remaining_seconds) end,
    updated_at = now()
from axiom_user_license_rollup r
where l.id = r.canonical_id;

update public.axiom_orders o set license_id = r.canonical_id
from public.axiom_licenses old, axiom_user_license_rollup r
where o.license_id = old.id and old.user_id = r.user_id
  and old.id <> r.canonical_id;

update public.axiom_trials t set license_id = r.canonical_id
from axiom_user_license_rollup r
where t.user_id = r.user_id and t.license_id <> r.canonical_id;

update public.axiom_activation_codes a set license_id = r.canonical_id
from public.axiom_licenses old, axiom_user_license_rollup r
where a.license_id = old.id and old.user_id = r.user_id
  and old.id <> r.canonical_id;

update public.axiom_licenses l set
    enabled = false, user_id = null, updated_at = now()
from axiom_user_license_rollup r
where l.user_id = r.user_id and l.id <> r.canonical_id;

create unique index if not exists axiom_one_license_per_user_idx
    on public.axiom_licenses(user_id) where user_id is not null;

create or replace function public.axiom_claim_trial(
    p_telegram_user_id bigint,
    p_key_hash text,
    p_key_ciphertext text
) returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
    v_user public.axiom_users%rowtype;
    v_trial public.axiom_trials%rowtype;
    v_license public.axiom_licenses%rowtype;
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
        select * into v_license from public.axiom_licenses
          where id = v_trial.license_id;
        return jsonb_build_object(
            'created', false, 'license_id', v_license.id,
            'key_ciphertext', v_license.key_ciphertext,
            'expires_at', v_license.expires_at);
    end if;

    select * into v_license from public.axiom_licenses
      where user_id = v_user.id for update;
    if found then
        return jsonb_build_object(
            'created', false, 'unavailable', true,
            'reason', 'existing_license');
    end if;

    insert into public.axiom_licenses(
        key_hash, label, expires_at, max_devices, user_id, key_ciphertext,
        source_kind, source_id
    ) values (
        p_key_hash, 'Telegram trial ' || p_telegram_user_id, v_expires_at, 1,
        v_user.id, p_key_ciphertext, 'trial', v_user.id
    ) returning * into v_license;
    insert into public.axiom_trials(user_id, license_id, expires_at)
    values (v_user.id, v_license.id, v_expires_at);
    return jsonb_build_object(
        'created', true, 'license_id', v_license.id,
        'key_ciphertext', v_license.key_ciphertext,
        'expires_at', v_license.expires_at);
end;
$$;

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
    v_created boolean := false;
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
            'accepted', true, 'idempotent', true, 'created_license', false,
            'activation', to_jsonb(v_code), 'license', to_jsonb(v_license));
    end if;

    select * into v_plan from public.axiom_plans where code = v_code.plan_code;
    if not found then raise exception 'activation plan unavailable'; end if;

    select * into v_license from public.axiom_licenses
      where user_id = v_user.id for update;
    if found then
        v_expires_at := case
            when v_license.expires_at is null then null
            else greatest(v_license.expires_at, now())
                 + make_interval(days => v_plan.duration_days)
        end;
        update public.axiom_licenses set
            expires_at = v_expires_at, enabled = true, updated_at = now()
          where id = v_license.id returning * into v_license;
    else
        v_created := true;
        v_expires_at := now() + make_interval(days => v_plan.duration_days);
        insert into public.axiom_licenses(
            key_hash, label, expires_at, max_devices, user_id, key_ciphertext,
            source_kind, source_id
        ) values (
            p_key_hash, 'FunPay activation ' || v_code.id, v_expires_at, 1,
            v_user.id, p_key_ciphertext, 'activation', v_code.id
        ) returning * into v_license;
    end if;

    update public.axiom_activation_codes set
        status = 'redeemed', redeemed_by_user_id = v_user.id,
        license_id = v_license.id, redeemed_at = now()
      where id = v_code.id returning * into v_code;

    return jsonb_build_object(
        'accepted', true, 'idempotent', false, 'created_license', v_created,
        'activation', to_jsonb(v_code), 'license', to_jsonb(v_license));
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

    select * into v_license from public.axiom_licenses
      where user_id = v_order.user_id for update;
    if found then
        v_expires_at := case
            when v_license.expires_at is null then null
            else greatest(v_license.expires_at, now())
                 + make_interval(days => v_order.duration_days)
        end;
        update public.axiom_licenses set
            expires_at = v_expires_at, enabled = true, updated_at = now()
          where id = v_license.id returning * into v_license;
    else
        v_expires_at := now() + make_interval(days => v_order.duration_days);
        insert into public.axiom_licenses(
            key_hash, label, expires_at, max_devices, user_id, key_ciphertext,
            source_kind, source_id
        ) values (
            p_key_hash, 'Telegram order ' || v_order.id, v_expires_at, 1,
            v_order.user_id, p_key_ciphertext, 'order', v_order.id
        ) returning * into v_license;
    end if;

    update public.axiom_orders set
        status = 'fulfilled', license_id = v_license.id,
        paid_at = now(), fulfilled_at = now(), updated_at = now()
      where id = v_order.id returning * into v_order;
    return jsonb_build_object('accepted', true, 'idempotent', false,
                              'order', to_jsonb(v_order), 'license', to_jsonb(v_license));
end;
$$;

revoke all on function public.axiom_claim_trial(bigint,text,text),
    public.axiom_redeem_activation_code(bigint,text,text,text),
    public.axiom_fulfill_paid_order(uuid,text,text,text,integer,text,text,text)
    from public, anon, authenticated;
grant execute on function public.axiom_claim_trial(bigint,text,text),
    public.axiom_redeem_activation_code(bigint,text,text,text),
    public.axiom_fulfill_paid_order(uuid,text,text,text,integer,text,text,text)
    to service_role;
