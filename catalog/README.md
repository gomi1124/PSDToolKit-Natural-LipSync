# AviUtl2カタログ登録資料

`registration.json`は、AviUtl2カタログの「パッケージ登録」画面にあるJSON入力へ貼り付けるためのsource bundleです。

登録用JSONには次の画像のGitHub Raw URLを設定しています。

- サムネイル: `images/thumbnail.png`（206x206）
- 説明画像: `images/detail.png`（1280x720）

インストーラーにはGitHub Releasesの`.au2pkg.zip`を指定しています。バージョン検出対象は`LipSyncAviUtl1.mod2`です。新しいバージョンを公開するときは、リリース日・バージョン・XXH3-128を更新してください。

画像の再生成にはPillowが必要です。

```powershell
py -3 .\catalog\generate_assets.py
```
