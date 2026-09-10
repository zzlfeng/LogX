package com.logx.tester.engine

object TestKeys {
    // 32 字节测试密钥: 000102...1f
    val BUILTIN_KEY_BYTES: ByteArray = ByteArray(32) { it.toByte() }
    const val DEFAULT_KEY_HEX: String = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"

    fun hexToBytes(hex: String): ByteArray? {
        val clean = hex.trim()
        if (clean.length != 64) return null
        return try {
            ByteArray(32) { i ->
                clean.substring(i * 2, i * 2 + 2).toInt(16).toByte()
            }
        } catch (e: Exception) {
            null
        }
    }

    fun bytesToHex(bytes: ByteArray): String {
        return bytes.joinToString("") { "%02x".format(it) }
    }
}

