create table if not exists public.secure_telemetry (
  singleton boolean primary key default true check (singleton),
  data jsonb not null,
  updated_at timestamptz not null default now()
);

create table if not exists public.secure_control_command (
  singleton boolean primary key default true check (singleton),
  command_id uuid not null unique,
  action text not null check (action in ('on', 'off')),
  created_at timestamptz not null,
  expires_at timestamptz not null
);

alter table public.secure_telemetry enable row level security;
alter table public.secure_control_command enable row level security;

revoke all on public.secure_telemetry from anon, authenticated;
revoke all on public.secure_control_command from anon, authenticated;
grant select, insert, update, delete on public.secure_telemetry to service_role;
grant select, insert, update, delete on public.secure_control_command to service_role;

create or replace function public.update_secure_telemetry_updated_at()
returns trigger
language plpgsql
as $$
begin
  new.updated_at = now();
  return new;
end;
$$;

drop trigger if exists secure_telemetry_updated_at on public.secure_telemetry;
create trigger secure_telemetry_updated_at
before update on public.secure_telemetry
for each row execute function public.update_secure_telemetry_updated_at();
