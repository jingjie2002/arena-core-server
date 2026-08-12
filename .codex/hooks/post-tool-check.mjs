import { existsSync } from "fs";
import { join, resolve } from "path";
import {
  fileExtension,
  getFilePath,
  note,
  projectRoot,
  readStdinJson,
  spawnLocal,
} from "./common.mjs";

const { data } = readStdinJson();
if (!data) process.exit(0);

const filePath = getFilePath(data);
if (!filePath) process.exit(0);

const absolute = resolve(projectRoot, filePath);
if (!absolute.startsWith(projectRoot) || !existsSync(absolute)) process.exit(0);

const formatExtensions = new Set([
  ".js", ".jsx", ".ts", ".tsx", ".json", ".css", ".scss", ".md", ".html", ".yml", ".yaml",
]);
if (!formatExtensions.has(fileExtension(absolute))) process.exit(0);

const win = process.platform === "win32";
const prettier = join(projectRoot, "node_modules", ".bin", win ? "prettier.cmd" : "prettier");
const biome = join(projectRoot, "node_modules", ".bin", win ? "biome.cmd" : "biome");

if (existsSync(prettier)) {
  const result = spawnLocal(prettier, ["--write", absolute], 10000);
  if (result.status === 0) note(`Harness: 已用 prettier 格式化 ${filePath}`);
  process.exit(0);
}

if (existsSync(biome)) {
  const result = spawnLocal(biome, ["format", "--write", absolute], 10000);
  if (result.status === 0) note(`Harness: 已用 biome 格式化 ${filePath}`);
  process.exit(0);
}

process.exit(0);
