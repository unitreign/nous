// Vercel serverless function — proxies the latest Nous .bin from GitHub.
// Runs server-side so GitHub's missing CORS headers are irrelevant.
export default async function handler(req, res) {
  res.setHeader('Access-Control-Allow-Origin', '*');
  if (req.method === 'OPTIONS') { res.status(204).end(); return; }

  try {
    const meta = await fetch('https://api.github.com/repos/unitreign/nous/releases/latest', {
      headers: { 'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28' },
    }).then(r => { if (!r.ok) throw new Error(`GitHub API: ${r.status}`); return r.json(); });

    const asset = meta.assets?.find(a => a.name.endsWith('.bin'));
    if (!asset) { res.status(404).json({ error: 'No .bin asset in latest release.' }); return; }

    const bin = await fetch(asset.browser_download_url, {
      headers: { 'Accept': 'application/octet-stream' },
      redirect: 'follow',
    });
    if (!bin.ok) { res.status(502).json({ error: `GitHub asset fetch failed: ${bin.status}` }); return; }

    res.setHeader('Content-Type', 'application/octet-stream');
    res.setHeader('Content-Disposition', `attachment; filename="${asset.name}"`);
    res.setHeader('X-Firmware-Version', meta.tag_name || '');
    res.setHeader('X-Firmware-Name', asset.name);
    res.send(Buffer.from(await bin.arrayBuffer()));
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
}
