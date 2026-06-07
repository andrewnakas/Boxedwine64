import { createReadStream } from "node:fs";
import { stat } from "node:fs/promises";
import { createServer } from "node:http";
import { extname, join, normalize, resolve, sep } from "node:path";

const root = resolve(import.meta.dirname);
const port = Number(process.argv[2] || 8000);
const host = process.argv[3] || "127.0.0.1";

const mimeTypes = new Map([
    [".css", "text/css; charset=utf-8"],
    [".html", "text/html; charset=utf-8"],
    [".js", "text/javascript; charset=utf-8"],
    [".mjs", "text/javascript; charset=utf-8"],
    [".wasm", "application/wasm"],
    [".zip", "application/zip"],
]);

function resolveRequestPath(url) {
    const { pathname } = new URL(url, "http://localhost");
    const relative = normalize(decodeURIComponent(pathname)).replace(/^(\.\.[/\\])+/, "");
    const absolute = join(root, relative === sep ? "boxedwine.html" : relative);
    if (absolute !== root && !absolute.startsWith(root + sep)) {
        return null;
    }
    return absolute;
}

const server = createServer(async (request, response) => {
    const filePath = resolveRequestPath(request.url || "/");
    if (!filePath) {
        response.writeHead(403);
        response.end("Forbidden");
        return;
    }

    let info;
    try {
        info = await stat(filePath);
    } catch (error) {
        response.writeHead(404);
        response.end("Not found");
        return;
    }

    const finalPath = info.isDirectory() ? join(filePath, "boxedwine.html") : filePath;
    const finalInfo = info.isDirectory() ? await stat(finalPath) : info;
    // The rootfs is immutable content addressed by name — let the browser cache it
    // long-term so an app switch (which reloads the page) re-reads the ~200MB
    // rootfs from cache instead of re-downloading. The page/launcher/wasm stay
    // revalidated so code changes show up on reload.
    const ext = extname(finalPath);
    const immutable = ext === ".zip" || ext === ".part" || /\.zip\.part\d+$/.test(finalPath);
    const headers = {
        "Content-Length": finalInfo.size,
        "Content-Type": mimeTypes.get(ext) || "application/octet-stream",
        "Cross-Origin-Embedder-Policy": "require-corp",
        "Cross-Origin-Opener-Policy": "same-origin",
        "Cross-Origin-Resource-Policy": "same-origin",
        "Cache-Control": immutable ? "public, max-age=31536000, immutable" : "no-cache",
    };
    response.writeHead(200, headers);
    createReadStream(finalPath).pipe(response);
});

server.listen(port, host, () => {
    console.log(`Serving ${root} at http://${host}:${port}/`);
});
