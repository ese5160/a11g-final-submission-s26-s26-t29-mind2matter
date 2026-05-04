FROM nodered/node-red:latest

COPY package.json /data/package.json
RUN cd /data && npm install --unsafe-perm --no-update-notifier --no-fund --omit=dev

COPY flows.json /data/flows.json
COPY settings.js /data/settings.js
