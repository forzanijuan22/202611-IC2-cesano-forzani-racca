FROM python:3.12.7-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY simulador_esp32.py .

CMD ["python", "-u", "simulador_esp32.py", "--broker", "mosquitto"]
