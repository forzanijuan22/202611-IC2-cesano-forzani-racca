FROM python:3.12.7-slim

WORKDIR /app

COPY backend/requirements.txt backend/requirements.txt
RUN pip install --no-cache-dir -r backend/requirements.txt

# server.py sirve el dashboard desde ../frontend
COPY backend/ backend/
COPY frontend/ frontend/

WORKDIR /app/backend
EXPOSE 8000
CMD ["python", "-u", "server.py"]
