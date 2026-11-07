// Prints a signing password that DevEco Studio stored encrypted in a
// build-profile.json5 signingConfigs entry.
//
//   node deveco_password.js <signing material dir> <encrypted hex>
//
// hvigor decrypts these itself; the Java hap-sign-tool, which `ohos-wl hap`
// needs to sign a HAP that carries a native package, takes them in plain
// text. The scheme (a root key from the material's fd/ and ac/ parts,
// unwrapping the work key in ce/, then AES-128-GCM) follows the published
// description in TermonyHQ/Termony's sign.js (MIT).

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const COMPONENT = Buffer.from([49, 243, 9, 115, 214, 175, 91, 184, 211, 190, 177, 88, 101, 131, 192, 119]);

function firstFile(dir) {
  const names = fs.readdirSync(dir).filter((name) => name !== ".DS_Store").sort();
  return fs.readFileSync(path.join(dir, names[0]));
}

// Layout: the length of the ciphertext with its tag (4 bytes, big endian),
// the IV, the ciphertext, and the 16-byte GCM tag.
function decrypt(key, data) {
  const ivLength = data.length - 4 - data.readUInt32BE(0);
  const iv = data.subarray(4, 4 + ivLength);
  const tag = data.subarray(data.length - 16);
  const decipher = crypto.createDecipheriv("aes-128-gcm", key, iv);
  decipher.setAuthTag(tag);
  return Buffer.concat([decipher.update(data.subarray(4 + ivLength, data.length - 16)), decipher.final()]);
}

function workKey(materialDir) {
  const fdDir = path.join(materialDir, "fd");
  const parts = fs.readdirSync(fdDir).filter((name) => name !== ".DS_Store").sort()
    .map((name) => firstFile(path.join(fdDir, name)));
  parts.push(COMPONENT);
  const mixed = Buffer.alloc(COMPONENT.length);
  for (let i = 0; i < mixed.length; i++) mixed[i] = parts.reduce((value, part) => value ^ part[i], 0);
  // DevEco passes the XORed bytes to PBKDF2 as a UTF-8 decoded string.
  const rootKey = crypto.pbkdf2Sync(mixed.toString(), firstFile(path.join(materialDir, "ac")), 10000, 16, "sha256");
  return decrypt(rootKey, firstFile(path.join(materialDir, "ce")));
}

const [materialParent, encrypted] = process.argv.slice(2);
if (!materialParent || !encrypted) {
  console.error("usage: deveco_password.js <signing material dir> <encrypted hex>");
  process.exit(2);
}
const key = workKey(path.join(materialParent, "material"));
process.stdout.write(decrypt(key, Buffer.from(encrypted, "hex")).toString("utf-8"));
