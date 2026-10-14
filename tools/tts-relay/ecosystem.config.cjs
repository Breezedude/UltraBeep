/* pm2 start ecosystem.config.cjs; pm2 save */
module.exports = {
  apps: [
    {
      name: "tts-relay",
      script: "server.js",
      cwd: __dirname,
      instances: 1,
      exec_mode: "fork",
      max_memory_restart: "150M",
      restart_delay: 2000,
      env: { NODE_ENV: "production" },
    },
  ],
};
