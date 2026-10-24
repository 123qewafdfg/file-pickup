# 发布产物

本目录存放**对外发布的安装包与校验值**。二进制文件不进入版本库（见 `.gitignore`），
正式分发请上传到仓库的 **Releases** 页面，本目录只保留说明与校验值。

| 文件 | 说明 |
| --- | --- |
| `FilePickup-v0.2.0-Setup.exe` | Windows 安装包（推荐） |
| `FilePickup-v0.2.0-Portable.zip` | 免安装便携版 |
| `SHA256SUMS.txt` | 上述产物的 SHA-256 |

## 校验

```powershell
Get-FileHash .\FilePickup-v0.2.0-Setup.exe -Algorithm SHA256
# 与 SHA256SUMS.txt 中对应行比对
```

## 上传到 GitHub Releases

```powershell
gh release create v0.2.0 .\release\FilePickup-v0.2.0-Setup.exe `
  .\release\FilePickup-v0.2.0-Portable.zip `
  --title "v0.2.0" --notes-file .\CHANGELOG.md
```
