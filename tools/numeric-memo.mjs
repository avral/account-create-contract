#!/usr/bin/env node
// Builds the digits-only memo the contract accepts, and is the reference for
// front ends that build it themselves:
//
//   node tools/numeric-memo.mjs vasya.ac PUB_K1_4yTp9kp2JsTudNPiZsJwTEdtK4QqAceoKwwMNYNTx1cMpFTRgW
//
// memo = name as uint64, zero-padded to 20 digits
//      + (33 key bytes || 4 checksum bytes) as one number, zero-padded to 88 digits
//
// The second half is exactly the number PUB_K1_'s base58 spells, so a key
// carries its checksum over unchanged.

const BASE58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
const NAME_CHARS = '.12345abcdefghijklmnopqrstuvwxyz'

function nameToUint64(name) {
  if (!/^[.1-5a-z]{1,12}$/.test(name)) throw new Error(`not an account name: ${name}`)

  let value = 0n
  for (let i = 0; i < name.length; i++) {
    value |= BigInt(NAME_CHARS.indexOf(name[i])) << BigInt(64 - 5 * (i + 1))
  }
  return value
}

function base58ToBigInt(text) {
  let value = 0n
  for (const c of text) {
    const digit = BASE58.indexOf(c)
    if (digit < 0) throw new Error(`not base58: ${c}`)
    value = value * 58n + BigInt(digit)
  }
  return value
}

export function numericMemo(account, key) {
  if (!key.startsWith('PUB_K1_')) throw new Error('the key must be PUB_K1_')

  return (
    nameToUint64(account).toString().padStart(20, '0') +
    base58ToBigInt(key.slice('PUB_K1_'.length)).toString().padStart(88, '0')
  )
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [account, key] = process.argv.slice(2)
  if (!account || !key) {
    console.error('usage: numeric-memo.mjs <nick>.ac PUB_K1_...')
    process.exit(1)
  }
  console.log(numericMemo(account, key))
}
