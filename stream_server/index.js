const path = require('node:path');
const express = require('express');
const app = express();

const port = 3008;

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

app.listen(port, () => {
	console.log("Listening on port", port);
})
