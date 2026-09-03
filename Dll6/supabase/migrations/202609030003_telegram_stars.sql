-- Telegram Stars pricing. Product prices remain denominated in RUB while each
-- order snapshots the rate and exact XTR amount used by its Telegram invoice.

insert into public.axiom_plans(
    code, title, duration_days, amount_minor, currency, active, sort_order
) values
    ('three_days', '3 дня', 3, 19000, 'RUB', true, 10),
    ('week', '7 дней', 7, 29000, 'RUB', true, 20),
    ('month', '30 дней', 30, 99000, 'RUB', true, 30)
on conflict (code) do update set
    title = excluded.title,
    duration_days = excluded.duration_days,
    amount_minor = excluded.amount_minor,
    currency = excluded.currency,
    active = excluded.active,
    sort_order = excluded.sort_order;

-- These legacy seed plans are not part of the currently approved storefront.
update public.axiom_plans set active = false
where code in ('quarter', 'year');

create table if not exists public.axiom_payment_settings (
    singleton boolean primary key default true check (singleton),
    rub_per_star numeric(10,4) not null check (rub_per_star > 0),
    rate_source text not null default 'manual_starfall'
        check (length(rate_source) between 1 and 64),
    updated_at timestamptz not null default now()
);

insert into public.axiom_payment_settings(singleton, rub_per_star, rate_source)
values (true, 1.2500, 'manual_starfall')
on conflict (singleton) do nothing;

alter table public.axiom_payment_settings enable row level security;
revoke all on public.axiom_payment_settings from public, anon, authenticated;
grant select, insert, update, delete on public.axiom_payment_settings to service_role;

alter table public.axiom_orders
    add column if not exists price_rub_minor integer
        check (price_rub_minor is null or price_rub_minor > 0),
    add column if not exists rub_per_star numeric(10,4)
        check (rub_per_star is null or rub_per_star > 0);

create or replace function public.axiom_create_star_order(
    p_telegram_user_id bigint,
    p_plan_code text,
    p_telegram_update_id bigint
) returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
    v_user_id uuid;
    v_plan public.axiom_plans%rowtype;
    v_order public.axiom_orders%rowtype;
    v_rate numeric(10,4);
    v_stars integer;
begin
    if p_telegram_user_id <= 0 or p_telegram_update_id <= 0 then
        raise exception 'invalid Telegram identifiers';
    end if;
    perform pg_advisory_xact_lock(p_telegram_update_id);

    select * into v_order from public.axiom_orders
      where telegram_update_id = p_telegram_update_id;
    if found then
        select id into v_user_id from public.axiom_users
          where telegram_user_id = p_telegram_user_id;
        if v_order.user_id <> v_user_id or v_order.plan_code <> p_plan_code
           or v_order.provider <> 'telegram_stars' then
            raise exception 'Telegram update already belongs to another order';
        end if;
        return to_jsonb(v_order);
    end if;

    select id into v_user_id from public.axiom_users
      where telegram_user_id = p_telegram_user_id;
    if v_user_id is null then raise exception 'Telegram user not registered'; end if;

    select * into v_plan from public.axiom_plans
      where code = p_plan_code and active for share;
    if not found or v_plan.currency <> 'RUB' then
        raise exception 'plan unavailable';
    end if;

    select rub_per_star into v_rate from public.axiom_payment_settings
      where singleton for share;
    if v_rate is null or v_rate <= 0 then
        raise exception 'Stars exchange rate unavailable';
    end if;
    v_stars := ceil(v_plan.amount_minor / (v_rate * 100))::integer;

    insert into public.axiom_orders(
        user_id, plan_code, duration_days, amount_minor, currency, provider,
        telegram_update_id, price_rub_minor, rub_per_star
    ) values (
        v_user_id, v_plan.code, v_plan.duration_days, v_stars, 'XTR',
        'telegram_stars', p_telegram_update_id, v_plan.amount_minor, v_rate
    ) returning * into v_order;
    return to_jsonb(v_order);
end;
$$;

revoke all on function public.axiom_create_star_order(bigint,text,bigint)
    from public, anon, authenticated;
grant execute on function public.axiom_create_star_order(bigint,text,bigint)
    to service_role;
