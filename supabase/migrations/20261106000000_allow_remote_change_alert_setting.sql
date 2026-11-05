alter table public.secure_control_command
  add column if not exists value double precision;

alter table public.secure_control_command
  drop constraint if exists secure_control_command_action_check;

alter table public.secure_control_command
  add constraint secure_control_command_action_check
  check (action in ('on', 'off', 'set-change-alert'));

alter table public.secure_control_command
  drop constraint if exists secure_control_command_target_check;

alter table public.secure_control_command
  add constraint secure_control_command_target_check
  check (target in ('led', 'buzzer', 'settings'));

alter table public.secure_control_command
  drop constraint if exists secure_control_command_value_check;

alter table public.secure_control_command
  add constraint secure_control_command_value_check
  check (
    (action in ('on', 'off') and target in ('led', 'buzzer') and value is null)
    or (
      action = 'set-change-alert'
      and target = 'settings'
      and value is not null
      and value between 0.5 and 20
      and value * 2 = trunc(value * 2)
    )
  );
