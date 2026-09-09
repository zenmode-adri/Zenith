#!/bin/bash
# supervise-build.sh — bucle supervisor del build dentro de WSL.
# Re-lanza make tras cada corte de WSL hasta que la imagen exista o haya demasiados intentos.
LOG=/home/epsil/aurknix-build.log
cd /home/epsil/aurknix || exit 1

for i in $(seq 1 60); do
  if ls /home/epsil/aurknix/target/*.img* /home/epsil/aurknix/release/*.img* 2>/dev/null | grep -q .; then
    echo "$(date) IMAGEN_EXISTE, fin del supervisor" >> "$LOG"
    exit 0
  fi
  echo "===== $(date) supervisor: intento $i =====" >> "$LOG"
  make RK3326 >> "$LOG" 2>&1
  RC=$?
  echo "===== $(date) supervisor: make termino rc=$RC =====" >> "$LOG"
  [ $RC -eq 0 ] && exit 0
  sleep 10
done
echo "$(date) supervisor: agotados 60 intentos" >> "$LOG"
exit 1
