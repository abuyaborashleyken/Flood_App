alter table public.secure_control_command
  add column if not exists target text not null default 'led';

alter table public.secure_control_command
  drop constraint if exists secure_control_command_target_check;

alter table public.secure_control_command
  add constraint secure_control_command_target_check
  check (target in ('led', 'buzzer'));
