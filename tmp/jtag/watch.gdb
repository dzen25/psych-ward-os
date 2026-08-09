
# GDB-сценарий для cpu0 (usb_driver и всё, кроме wifi_driver, работает
# там без явной привязки - main.cpp:1421-1424, см. tmp/jtag_run.sh).
# Цель/символы подключаются через -ex ДО этого файла. Одна непрерывная
# сессия: подключаемся ЗАДОЛГО до того, как usb_driver дойдёт до
# крашащего кода (90с пауза в usb_driver.cpp/main()), ставим все
# breakpoint'ы и просто ждём через continue - обычные IRQ/syscall'ы по
# пути не мешают (не наши breakpoint'ы), а сам abort поймается ровно
# там, где случится, без гонки по времени и без переподключения к уже
# наступившему краху (см. ROADMAP.md "Пятнадцатая попытка" про то, почему
# переподключение постфактум ловило случайную точку и потом не давало
# ставить breakpoint'ы - "Cannot access memory").
set pagination off
set confirm off

break *0x40233c
break *0x401494
break *0xffffff8000010784
break *0xffffff80000107e8
break *0xffffff8000010940
break *0xffffff80000140c0

printf "\n===== WAIT FOR STOP #1 (run_bring_up) =====\n"
continue
printf "\n===== STOP #1 =====\n"
info symbol $pc
p/x $pc
info registers
bt

printf "\n===== CONTINUE TO STOP #2 (step1_read_capabilities) =====\n"
continue
printf "\n===== STOP #2 =====\n"
info symbol $pc
p/x $pc
info registers

printf "\n===== ПОШАГОВО ЧЕРЕЗ ЧТЕНИЕ CAPABILITY-РЕГИСТРОВ (nexti, шаг через вызовы печати) =====\n"
set $i = 0
while $i < 25
  nexti
  printf "  step %d pc=", $i
  p/x $pc
  info symbol $pc
  set $i = $i + 1
end

printf "\n===== CONTINUE - ЛОВЛЯ ОБРАБОТЧИКА FAULT'А (если ещё не там) =====\n"
continue
printf "\n===== STOP #3 (возможно, обработчик SError) =====\n"
info symbol $pc
p/x $pc
info registers
printf "\n----- system regs (несколько вариантов имён, что сработает) -----\n"
info all-registers
p/x $ESR_EL1
p/x $esr_el1
p/x $FAR_EL1
p/x $far_el1
p/x $ELR_EL1
p/x $elr_el1
printf "\n----- backtrace / disas -----\n"
bt
x/8i $pc-16
