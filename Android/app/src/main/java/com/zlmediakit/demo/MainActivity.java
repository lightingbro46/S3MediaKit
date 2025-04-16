package com.s3mediakit.demo;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Environment;
import android.support.v4.app.ActivityCompat;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

import com.s3mediakit.jni.S3MediaKit;

public class MainActivity extends AppCompatActivity {
    public static final String TAG = "S3MediaKit";
    private static String[] PERMISSIONS_STORAGE = {
            "android.permission.READ_EXTERNAL_STORAGE",
            "android.permission.WRITE_EXTERNAL_STORAGE",
            "android.permission.INTERNET"};

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        boolean permissionSuccess = true;
        for(String str : PERMISSIONS_STORAGE){
            int permission = ActivityCompat.checkSelfPermission(this, str);
            if (permission != PackageManager.PERMISSION_GRANTED) {
                // If you do not have permission to write, you will apply for permission to write and a dialog box will pop up.
                ActivityCompat.requestPermissions(this, PERMISSIONS_STORAGE,1);
                permissionSuccess = false;
                break;
            }
        }

        String sd_dir = Environment.getExternalStoragePublicDirectory("").toString();
        if(permissionSuccess){
            Toast.makeText(this,"You can modify the configuration file and start: " + sd_dir + "/s3mediakit.ini" ,Toast.LENGTH_LONG).show();
            Toast.makeText(this,"Please place the SSL certificate in: " + sd_dir + "/s3mediakit.pem" ,Toast.LENGTH_LONG).show();
        }else{
            Toast.makeText(this,"Please give me permissions, otherwise the test will not be started！" ,Toast.LENGTH_LONG).show();
        }
        S3MediaKit.startDemo(sd_dir);
    }

    private S3MediaKit.MediaPlayer _player;
    private void test_player(){
        _player = new S3MediaKit.MediaPlayer("rtmp://live.hkstv.hk.lxdns.com/live/hks1", new S3MediaKit.MediaPlayerCallBack() {
            @Override
            public void onPlayResult(int code, String msg) {
                Log.d(TAG,"onPlayResult:" + code + "," + msg);
            }

            @Override
            public void onShutdown(int code, String msg) {
                Log.d(TAG,"onShutdown:" + code + "," + msg);
            }

            @Override
            public void onData(S3MediaKit.MediaFrame frame) {
                Log.d(TAG,"onData:"
                        + frame.trackType + ","
                        + frame.codecId + ","
                        + frame.dts + ","
                        + frame.pts + ","
                        + frame.keyFrame + ","
                        + frame.prefixSize + ","
                        + frame.data.length);
            }
        });
    }

}
