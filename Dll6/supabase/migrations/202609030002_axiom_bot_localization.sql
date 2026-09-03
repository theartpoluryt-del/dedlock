alter table public.axiom_users
    add column if not exists language_code text not null default 'ru'
    check (language_code in ('ru', 'en'));

create or replace function public.axiom_set_telegram_language(
    p_telegram_user_id bigint,
    p_language_code text
) returns text
language plpgsql security definer set search_path = public, pg_temp as $$
begin
    if p_language_code not in ('ru', 'en') then
        raise exception 'unsupported language';
    end if;

    update public.axiom_users
       set language_code = p_language_code,
           updated_at = now()
     where telegram_user_id = p_telegram_user_id;

    if not found then raise exception 'Telegram user not registered'; end if;
    return p_language_code;
end;
$$;

revoke all on function public.axiom_set_telegram_language(bigint,text)
    from public, anon, authenticated;
grant execute on function public.axiom_set_telegram_language(bigint,text)
    to service_role;
