# Compiler integration

GCC本体を変更しない。GCCソース・本体バイナリ・GCCパッケージの変更、
それを前提とする再ビルド・配備・compiler forkは作業対象外とする。
既存plugin APIやdistcc側の実行時symbol interpositionを使い、GCCの
ファイル・命令列の書換え、非公開レイアウトの決め打ち、版固定表は導入しない。
