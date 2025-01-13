const https = require("https");
const fs = require("fs");
const path = require('node:path');
const express = require('express');
const app = express();
const HTTPS_PORT = 8443;

app.use('/public', express.static(path.join(__dirname, 'public'), {
	setHeaders: (res, filePath) => {
		if (filePath.endsWith('.m3u8')) {
			res.setHeader('Content-Type', 'application/vnd.apple.mpegurl');
			res.setHeader('Cache-Control', 'no-cache, no-store, must-revalidate');
			res.setHeader('Pragma', 'no-cache');
			res.setHeader('Expires', '0');
		} else if (filePath.endsWith('.ts')) {
			res.setHeader('Content-Type', 'video/MP2T');
		}
	}
}));

app.get('/', (req, res) => {
	res.sendFile(path.join(__dirname, '/index.html'));
})

const options = {
	key: fs.readFileSync(path.join(__dirname, 'key.pem')),
	cert: fs.readFileSync(path.join(__dirname, 'cert.pem')),
};

https.createServer(options, app).listen(HTTPS_PORT, () => {
	console.log(`Server running at https://localhost/${HTTPS_PORT}`);
});
