package com.walletclick.android;

import android.content.Context;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

final class Vault {
    private static final String ALIAS="WalletClick.PasswordKey.v1";
    private static SecretKey key() throws Exception {
        KeyStore store=KeyStore.getInstance("AndroidKeyStore"); store.load(null);
        if (!store.containsAlias(ALIAS)) {
            KeyGenerator gen=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            gen.init(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
            gen.generateKey();
        }
        return (SecretKey)store.getKey(ALIAS, null);
    }
    static void save(Context context, String wallet, char[] password) {
        byte[] bytes=null;
        try {
            if (password.length==0 || wallet.isEmpty()) throw new IllegalStateException();
            bytes=new String(password).getBytes(StandardCharsets.UTF_8);
            Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding"); cipher.init(Cipher.ENCRYPT_MODE, key());
            cipher.updateAAD(wallet.getBytes(StandardCharsets.UTF_8));
            String value=Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP)+":"+Base64.encodeToString(cipher.doFinal(bytes), Base64.NO_WRAP);
            if (!context.getSharedPreferences("vault", Context.MODE_PRIVATE).edit().putString(wallet,value).commit()) throw new IllegalStateException();
        } catch (Exception e) { throw new IllegalStateException("密码加密保存失败，请检查系统 Keystore"); }
        finally { if (bytes!=null) Arrays.fill(bytes,(byte)0); Arrays.fill(password,'\0'); }
    }
    static char[] load(Context context, String wallet) {
        byte[] bytes=null;
        try {
            String value=context.getSharedPreferences("vault",Context.MODE_PRIVATE).getString(wallet,null);
            if (value==null) throw new IllegalStateException();
            String[] parts=value.split(":");
            Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE,key(),new GCMParameterSpec(128,Base64.decode(parts[0],Base64.NO_WRAP)));
            cipher.updateAAD(wallet.getBytes(StandardCharsets.UTF_8)); bytes=cipher.doFinal(Base64.decode(parts[1],Base64.NO_WRAP));
            return new String(bytes,StandardCharsets.UTF_8).toCharArray();
        } catch(Exception e) { throw new IllegalStateException("没有可用的本机钱包密码，请重新设置"); }
        finally { if(bytes!=null) Arrays.fill(bytes,(byte)0); }
    }
    static void delete(Context context,String wallet) { context.getSharedPreferences("vault",Context.MODE_PRIVATE).edit().remove(wallet).apply(); }
    static boolean exists(Context context,String wallet) { return context.getSharedPreferences("vault",Context.MODE_PRIVATE).contains(wallet); }
}
