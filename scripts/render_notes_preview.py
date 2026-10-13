"""Render a local preview of the OkuFlow lecture-notes page.

Builds a notes page from the real template headers
(include/okuflow/common/notes_document_template.hpp and
notes_transcribe_bridge.hpp) plus sample entries whose markup mirrors
AssistiveRuntime (AppendNoteHtmlSection, AppendNoteMediaPair, the AI and
Assistant note paths). The output folder has the same layout as a real
OkuFlow data folder (Notes/, Recordings/<date>/, Photos/<date>/, images/), so
"Allow folder access" and "Transcribe" can be tested in Chrome by choosing the
preview folder itself.

Sample media is generated with ffmpeg when it is on PATH; otherwise the page
still renders, with missing media. Pass --speech to use your own WAV for the
recording's audio (the default synthesizes English speech with Windows SAPI
when available).

Usage:
    python scripts/render_notes_preview.py [--out DIR] [--lang <supported UI language>] [--speech file.wav]

The sample markup is a copy of the C++ markup: when AssistiveRuntime changes
the structure of an entry, update the matching sample below.
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "include" / "okuflow" / "common" / "notes_document_template.hpp"
BRIDGE = ROOT / "include" / "okuflow" / "common" / "notes_transcribe_bridge.hpp"

STRINGS = {'en': {'title': 'OkuFlow Lecture Notes',
        'started': 'Started %1',
        'conversation': 'Conversation',
        'expand': 'Expand all',
        'collapse': 'Collapse all',
        'theme': 'Light or dark colors',
        'video': 'Video recorded',
        'photo': 'Photo captured',
        'explain': 'Scene explanation',
        'read': 'Text on screen',
        'annotated': 'Annotated view',
        'orig_video': 'Original camera video',
        'proc_video': 'Processed camera video',
        'orig_photo': 'Original camera photo',
        'proc_photo': 'Processed camera photo',
        'pair_video': 'Original and processed video',
        'pair_photo': 'Original and processed photo',
        'open_orig': 'Open original video',
        'open_proc': 'Open processed video',
        'orig_view': 'Original camera view',
        'proc_view': 'Processed camera view',
        'analyzed': 'View the assistant analyzed',
        'captured_proc': 'Captured processed camera view'},
 'tr': {'title': 'OkuFlow Ders Notları',
        'started': 'Başlangıç %1',
        'conversation': 'Sohbet',
        'expand': 'Tümünü genişlet',
        'collapse': 'Tümünü daralt',
        'theme': 'Açık veya koyu renkler',
        'video': 'Video kaydedildi',
        'photo': 'Fotoğraf çekildi',
        'explain': 'Sahne açıklaması',
        'read': 'Ekrandaki metin',
        'annotated': 'İşaretli görünüm',
        'orig_video': 'Özgün kamera videosu',
        'proc_video': 'İşlenmiş kamera videosu',
        'orig_photo': 'Özgün kamera fotoğrafı',
        'proc_photo': 'İşlenmiş kamera fotoğrafı',
        'pair_video': 'Özgün ve işlenmiş video',
        'pair_photo': 'Özgün ve işlenmiş fotoğraf',
        'open_orig': 'Özgün videoyu aç',
        'open_proc': 'İşlenmiş videoyu aç',
        'orig_view': 'Özgün kamera görüntüsü',
        'proc_view': 'İşlenmiş kamera görüntüsü',
        'analyzed': 'Asistanın incelediği görüntü',
        'captured_proc': 'Yakalanan işlenmiş kamera görüntüsü'},
 'de': {'title': 'OkuFlow-Vorlesungsnotizen',
        'started': 'Begonnen %1',
        'conversation': 'Gespräch',
        'expand': 'Alle aufklappen',
        'collapse': 'Alle zuklappen',
        'theme': 'Helle oder dunkle Farben',
        'video': 'Video aufgenommen',
        'photo': 'Foto aufgenommen',
        'explain': 'Szenenbeschreibung',
        'read': 'Text auf dem Bildschirm',
        'annotated': 'Markierte Ansicht',
        'orig_video': 'Originales Kameravideo',
        'proc_video': 'Bearbeitetes Kameravideo',
        'orig_photo': 'Originales Kamerafoto',
        'proc_photo': 'Bearbeitetes Kamerafoto',
        'pair_video': 'Originales und bearbeitetes Video',
        'pair_photo': 'Originales und bearbeitetes Foto',
        'open_orig': 'Originalvideo öffnen',
        'open_proc': 'Bearbeitetes Video öffnen',
        'orig_view': 'Originale Kameraansicht',
        'proc_view': 'Bearbeitete Kameraansicht',
        'analyzed': 'Vom Assistenten analysierte Ansicht',
        'captured_proc': 'Aufgenommene bearbeitete Kameraansicht'},
 'fr': {'title': 'OkuFlow Notes de cours',
        'started': 'Démarré %1',
        'conversation': 'Conversation',
        'expand': 'Tout développer',
        'collapse': 'Tout réduire',
        'theme': 'Couleurs claires ou foncées',
        'video': 'Vidéo enregistrée',
        'photo': 'Photo prise',
        'explain': 'Explication de la scène',
        'read': "Texte à l'écran",
        'annotated': 'Vue annotée',
        'orig_video': 'Vidéo originale de la caméra',
        'proc_video': 'Vidéo de la caméra traitée',
        'orig_photo': "Photo originale de l'appareil photo",
        'proc_photo': "Photo d'appareil photo traitée",
        'pair_video': 'Vidéo originale et traitée',
        'pair_photo': 'Photo originale et retouchée',
        'open_orig': 'Ouvrir la vidéo originale',
        'open_proc': 'Ouvrir la vidéo traitée',
        'orig_view': 'Vue originale de la caméra',
        'proc_view': 'Vue de la caméra traitée',
        'analyzed': "Voir l'assistant analysé",
        'captured_proc': 'Vue de la caméra traitée capturée'},
 'es': {'title': 'OkuFlow Notas de clase',
        'started': 'Iniciado %1',
        'conversation': 'Conversación',
        'expand': 'Expandir todo',
        'collapse': 'Contraer todo',
        'theme': 'Colores claros u oscuros',
        'video': 'Vídeo grabado',
        'photo': 'Foto capturada',
        'explain': 'Explicación de la escena',
        'read': 'Texto en pantalla',
        'annotated': 'Vista comentada',
        'orig_video': 'Vídeo de cámara original.',
        'proc_video': 'Vídeo de cámara procesada',
        'orig_photo': 'Foto de cámara original',
        'proc_photo': 'Foto de cámara procesada',
        'pair_video': 'Vídeo original y procesado.',
        'pair_photo': 'Foto original y procesada.',
        'open_orig': 'Abrir vídeo original',
        'open_proc': 'Abrir vídeo procesado',
        'orig_view': 'Vista de cámara original',
        'proc_view': 'Vista de cámara procesada',
        'analyzed': 'Ver el asistente analizado',
        'captured_proc': 'Vista de cámara procesada capturada'},
 'it': {'title': 'OkuFlow Appunti delle lezioni',
        'started': 'Iniziato %1',
        'conversation': 'Conversazione',
        'expand': 'Espandi tutto',
        'collapse': 'Comprimi tutto',
        'theme': 'Colori chiari o scuri',
        'video': 'Video registrato',
        'photo': 'Foto catturata',
        'explain': 'Spiegazione della scena',
        'read': 'Testo sullo schermo',
        'annotated': 'Vista annotata',
        'orig_video': 'Video originale della fotocamera',
        'proc_video': 'Video della fotocamera elaborato',
        'orig_photo': 'Foto originale della fotocamera',
        'proc_photo': 'Foto della fotocamera elaborata',
        'pair_video': 'Video originale ed elaborato',
        'pair_photo': 'Foto originale ed elaborata',
        'open_orig': 'Apri il video originale',
        'open_proc': 'Apri il video elaborato',
        'orig_view': 'Vista originale della telecamera',
        'proc_view': 'Vista della telecamera elaborata',
        'analyzed': "Visualizza l'assistente analizzato",
        'captured_proc': 'Vista della telecamera elaborata acquisita'},
 'pt-BR': {'title': 'OkuFlow Notas de aula',
           'started': 'Iniciado %1',
           'conversation': 'Conversa',
           'expand': 'Expandir tudo',
           'collapse': 'Recolher tudo',
           'theme': 'Cores claras ou escuras',
           'video': 'Vídeo gravado',
           'photo': 'Foto capturada',
           'explain': 'Explicação da cena',
           'read': 'Texto na tela',
           'annotated': 'Visualização anotada',
           'orig_video': 'Vídeo original da câmera',
           'proc_video': 'Vídeo da câmera processado',
           'orig_photo': 'Foto original da câmera',
           'proc_photo': 'Foto da câmera processada',
           'pair_video': 'Vídeo original e processado',
           'pair_photo': 'Foto original e processada',
           'open_orig': 'Abra o vídeo original',
           'open_proc': 'Abrir vídeo processado',
           'orig_view': 'Visualização original da câmera',
           'proc_view': 'Visualização da câmera processada',
           'analyzed': 'Veja o assistente analisado',
           'captured_proc': 'Visualização da câmera processada capturada'},
 'ru': {'title': 'OkuFlow Конспекты лекций',
        'started': 'Началось %1',
        'conversation': 'Разговор',
        'expand': 'Развернуть все',
        'collapse': 'Свернуть все',
        'theme': 'Светлые или темные цвета',
        'video': 'Видео записано',
        'photo': 'Фотография сделана',
        'explain': 'Объяснение сцены',
        'read': 'Текст на экране',
        'annotated': 'Аннотированный вид',
        'orig_video': 'Оригинальное видео с камеры',
        'proc_video': 'Обработанное видео с камеры',
        'orig_photo': 'Оригинальное фото с камеры',
        'proc_photo': 'Обработанное фото с камеры',
        'pair_video': 'Оригинальное и обработанное видео',
        'pair_photo': 'Оригинальное и обработанное фото',
        'open_orig': 'Открыть оригинальное видео',
        'open_proc': 'Открыть обработанное видео',
        'orig_view': 'Исходный вид с камеры',
        'proc_view': 'Обработанный вид с камеры',
        'analyzed': 'Просмотрите анализ помощника',
        'captured_proc': 'Захваченный обработанный вид с камеры'},
 'uk': {'title': 'OkuFlow Конспекти лекцій',
        'started': 'Почато %1',
        'conversation': 'Розмова',
        'expand': 'Розгорнути все',
        'collapse': 'Згорнути все',
        'theme': 'Світлі або темні кольори',
        'video': 'Записано відео',
        'photo': 'Фото зроблено',
        'explain': 'Пояснення сцени',
        'read': 'Текст на екрані',
        'annotated': 'Анотований вигляд',
        'orig_video': 'Оригінальне відео з камери',
        'proc_video': 'Оброблено відео з камери',
        'orig_photo': 'Оригінальна фотокамера',
        'proc_photo': 'Оброблене фото з камери',
        'pair_video': 'Оригінальне та оброблене відео',
        'pair_photo': 'Оригінал і оброблена фотографія',
        'open_orig': 'Відкрити оригінальне відео',
        'open_proc': 'Відкрийте оброблене відео',
        'orig_view': 'Оригінальний вигляд камери',
        'proc_view': 'Оброблений вид камери',
        'analyzed': 'Переглянути проаналізований помічник',
        'captured_proc': 'Захоплений оброблений вигляд камери'},
 'ar': {'title': 'OkuFlow ملاحظات المحاضرة',
        'started': 'بدأت %1',
        'conversation': 'محادثة',
        'expand': 'قم بتوسيع الكل',
        'collapse': 'طي الكل',
        'theme': 'الألوان الفاتحة أو الداكنة',
        'video': 'تم تسجيل الفيديو',
        'photo': 'تم التقاط الصورة',
        'explain': 'شرح المشهد',
        'read': 'النص على الشاشة',
        'annotated': 'عرض مشروح',
        'orig_video': 'فيديو الكاميرا الأصلي',
        'proc_video': 'معالجة فيديو الكاميرا',
        'orig_photo': 'صورة الكاميرا الأصلية',
        'proc_photo': 'صورة الكاميرا المعالجة',
        'pair_video': 'الفيديو الأصلي والمعالج',
        'pair_photo': 'الصورة الأصلية والمعالجة',
        'open_orig': 'فتح الفيديو الأصلي',
        'open_proc': 'فتح الفيديو المعالج',
        'orig_view': 'عرض الكاميرا الأصلي',
        'proc_view': 'عرض الكاميرا المعالجة',
        'analyzed': 'عرض المساعد الذي تم تحليله',
        'captured_proc': 'تم التقاط عرض الكاميرا المعالجة'},
 'hi': {'title': 'OkuFlow व्याख्यान नोट्स',
        'started': 'प्रारंभ %1',
        'conversation': 'बातचीत',
        'expand': 'सभी का विस्तार करें',
        'collapse': 'सभी को संक्षिप्त करें',
        'theme': 'हल्के या गहरे रंग',
        'video': 'वीडियो रिकॉर्ड किया गया',
        'photo': 'फोटो खींच लिया गया',
        'explain': 'दृश्य स्पष्टीकरण',
        'read': 'स्क्रीन पर टेक्स्ट',
        'annotated': 'एनोटेट दृश्य',
        'orig_video': 'मूल कैमरा वीडियो',
        'proc_video': 'संसाधित कैमरा वीडियो',
        'orig_photo': 'मूल कैमरा फोटो',
        'proc_photo': 'संसाधित कैमरा फ़ोटो',
        'pair_video': 'मूल और संसाधित वीडियो',
        'pair_photo': 'मूल और संसाधित फ़ोटो',
        'open_orig': 'मूल वीडियो खोलें',
        'open_proc': 'संसाधित वीडियो खोलें',
        'orig_view': 'मूल कैमरा दृश्य',
        'proc_view': 'संसाधित कैमरा दृश्य',
        'analyzed': 'विश्लेषण किए गए सहायक को देखें',
        'captured_proc': 'संसाधित कैमरा दृश्य कैप्चर किया गया'},
 'ja': {'title': 'OkuFlow 講義ノート',
        'started': '%1 開始',
        'conversation': '会話',
        'expand': 'すべて展開',
        'collapse': 'すべて折りたたむ',
        'theme': '明るい色または暗い色',
        'video': '録画されたビデオ',
        'photo': '撮影した写真',
        'explain': '場面解説',
        'read': '画面上のテキスト',
        'annotated': '注釈付きビュー',
        'orig_video': 'オリジナルのカメラビデオ',
        'proc_video': '処理されたカメラビデオ',
        'orig_photo': 'オリジナルのカメラ写真',
        'proc_photo': '加工されたカメラ写真',
        'pair_video': 'オリジナルおよび加工されたビデオ',
        'pair_photo': 'オリジナル写真と加工済み写真',
        'open_orig': '元のビデオを開く',
        'open_proc': '処理されたビデオを開く',
        'orig_view': '元のカメラビュー',
        'proc_view': '処理されたカメラビュー',
        'analyzed': '分析されたアシスタントを表示する',
        'captured_proc': 'キャプチャされた処理済みのカメラ ビュー'},
 'ko': {'title': 'OkuFlow 강의노트',
        'started': '시작됨 %1',
        'conversation': '대화',
        'expand': '모두 펼치기',
        'collapse': '모두 접기',
        'theme': '밝은 색상 또는 어두운 색상',
        'video': '녹화된 영상',
        'photo': '캡처된 사진',
        'explain': '장면 설명',
        'read': '화면의 텍스트',
        'annotated': '주석이 달린 보기',
        'orig_video': '원본 카메라 영상',
        'proc_video': '처리된 카메라 영상',
        'orig_photo': '원본 카메라 사진',
        'proc_photo': '처리된 카메라 사진',
        'pair_video': '원본 및 처리된 동영상',
        'pair_photo': '원본 및 처리된 사진',
        'open_orig': '원본 영상 열기',
        'open_proc': '처리된 영상 열기',
        'orig_view': '원본 카메라 뷰',
        'proc_view': '처리된 카메라 보기',
        'analyzed': '분석된 어시스턴트 보기',
        'captured_proc': '캡처된 처리된 카메라 보기'},
 'zh-Hans': {'title': 'OkuFlow 讲义',
             'started': '开始%1',
             'conversation': '对话',
             'expand': '全部展开',
             'collapse': '全部折叠',
             'theme': '浅色或深色',
             'video': '已录制视频',
             'photo': '拍摄的照片',
             'explain': '场景讲解',
             'read': '屏幕上的文字',
             'annotated': '带注释的视图',
             'orig_video': '原始相机视频',
             'proc_video': '处理后的摄像机视频',
             'orig_photo': '原始相机照片',
             'proc_photo': '处理后的相机照片',
             'pair_video': '原始视频和处理后的视频',
             'pair_photo': '原始照片和经过处理的照片',
             'open_orig': '打开原视频',
             'open_proc': '打开处理后的视频',
             'orig_view': '原始相机视图',
             'proc_view': '处理后的相机视图',
             'analyzed': '查看助手分析',
             'captured_proc': '捕获并处理的相机视图'},
 'nl': {'title': 'OkuFlow Aantekeningen bij de lezing',
        'started': 'Gestart %1',
        'conversation': 'Gesprek',
        'expand': 'Alles uitvouwen',
        'collapse': 'Alles samenvouwen',
        'theme': 'Lichte of donkere kleuren',
        'video': 'Video opgenomen',
        'photo': 'Foto vastgelegd',
        'explain': 'Uitleg van scène',
        'read': 'Tekst op scherm',
        'annotated': 'Geannoteerde weergave',
        'orig_video': 'Originele cameravideo',
        'proc_video': 'Verwerkte cameravideo',
        'orig_photo': 'Originele camerafoto',
        'proc_photo': 'Verwerkte camerafoto',
        'pair_video': 'Originele en bewerkte video',
        'pair_photo': 'Originele en bewerkte foto',
        'open_orig': 'Originele video openen',
        'open_proc': 'Verwerkte video openen',
        'orig_view': 'Origineel camerabeeld',
        'proc_view': 'Verwerkt camerabeeld',
        'analyzed': 'Bekijk de assistent geanalyseerd',
        'captured_proc': 'Vastgelegd verwerkt camerabeeld'},
 'pl': {'title': 'OkuFlow Notatki z wykładów',
        'started': 'Rozpoczęto %1',
        'conversation': 'Rozmowa',
        'expand': 'Rozwiń wszystko',
        'collapse': 'Zwiń wszystko',
        'theme': 'Jasne lub ciemne kolory',
        'video': 'Film nagrany',
        'photo': 'Zdjęcie zrobione',
        'explain': 'Wyjaśnienie sceny',
        'read': 'Tekst na ekranie',
        'annotated': 'Widok z adnotacją',
        'orig_video': 'Oryginalne wideo z kamery',
        'proc_video': 'Przetworzone wideo z kamery',
        'orig_photo': 'Oryginalne zdjęcie z aparatu',
        'proc_photo': 'Przetworzone zdjęcie z aparatu',
        'pair_video': 'Oryginalne i przetworzone wideo',
        'pair_photo': 'Zdjęcie oryginalne i przetworzone',
        'open_orig': 'Otwórz oryginalne wideo',
        'open_proc': 'Otwórz przetworzone wideo',
        'orig_view': 'Oryginalny widok z kamery',
        'proc_view': 'Przetworzony widok z kamery',
        'analyzed': 'Zobacz analizowanego asystenta',
        'captured_proc': 'Przechwycony przetworzony widok z kamery'},
 'cs': {'title': 'OkuFlow Poznámky k přednášce',
        'started': 'Zahájeno %1',
        'conversation': 'Konverzace',
        'expand': 'Rozbalit vše',
        'collapse': 'Sbalit vše',
        'theme': 'Světlé nebo tmavé barvy',
        'video': 'Video natočeno',
        'photo': 'Fotka zachycena',
        'explain': 'Vysvětlení scény',
        'read': 'Text na obrazovce',
        'annotated': 'Komentovaný pohled',
        'orig_video': 'Originální video z fotoaparátu',
        'proc_video': 'Zpracované video z kamery',
        'orig_photo': 'Originální foto z fotoaparátu',
        'proc_photo': 'Zpracovaná fotografie z fotoaparátu',
        'pair_video': 'Originální a zpracované video',
        'pair_photo': 'Originální a zpracovaná fotografie',
        'open_orig': 'Otevřete původní video',
        'open_proc': 'Otevřete zpracované video',
        'orig_view': 'Původní pohled z kamery',
        'proc_view': 'Zpracovaný pohled kamery',
        'analyzed': 'Prohlédněte si analyzovaného asistenta',
        'captured_proc': 'Zachycený zpracovaný pohled kamery'},
 'sv': {'title': 'OkuFlow Föreläsningsanteckningar',
        'started': 'Startade %1',
        'conversation': 'Konversation',
        'expand': 'Utöka alla',
        'collapse': 'Komprimera alla',
        'theme': 'Ljusa eller mörka färger',
        'video': 'Video inspelad',
        'photo': 'Foto tagna',
        'explain': 'Scenförklaring',
        'read': 'Text på skärmen',
        'annotated': 'Kommenterad vy',
        'orig_video': 'Original kameravideo',
        'proc_video': 'Bearbetad kameravideo',
        'orig_photo': 'Original kamerafoto',
        'proc_photo': 'Bearbetat kamerafoto',
        'pair_video': 'Original och bearbetad video',
        'pair_photo': 'Original och bearbetat foto',
        'open_orig': 'Öppna originalvideon',
        'open_proc': 'Öppna bearbetad video',
        'orig_view': 'Original kameravy',
        'proc_view': 'Bearbetad kameravy',
        'analyzed': 'Se den analyserade assistenten',
        'captured_proc': 'Fångad bearbetad kameravy'},
 'id': {'title': 'OkuFlow Catatan Kuliah',
        'started': 'Dimulai %1',
        'conversation': 'Percakapan',
        'expand': 'Perluas semuanya',
        'collapse': 'Ciutkan semuanya',
        'theme': 'Warna terang atau gelap',
        'video': 'Video direkam',
        'photo': 'Foto diambil',
        'explain': 'Penjelasan adegan',
        'read': 'Teks di layar',
        'annotated': 'Tampilan beranotasi',
        'orig_video': 'Video kamera asli',
        'proc_video': 'Video kamera yang diproses',
        'orig_photo': 'Foto kamera asli',
        'proc_photo': 'Foto kamera yang diproses',
        'pair_video': 'Video asli dan olahan',
        'pair_photo': 'Foto asli dan olahan',
        'open_orig': 'Buka video asli',
        'open_proc': 'Buka video yang diproses',
        'orig_view': 'Tampilan kamera asli',
        'proc_view': 'Tampilan kamera yang diproses',
        'analyzed': 'Lihat asisten dianalisis',
        'captured_proc': 'Tampilan kamera yang diproses ditangkap'},
 'vi': {'title': 'OkuFlow Ghi chú bài giảng',
        'started': 'Đã bắt đầu %1',
        'conversation': 'Cuộc trò chuyện',
        'expand': 'Mở rộng tất cả',
        'collapse': 'Thu gọn tất cả',
        'theme': 'Màu sắc sáng hoặc tối',
        'video': 'Video đã được ghi lại',
        'photo': 'Đã chụp ảnh',
        'explain': 'Giải thích cảnh',
        'read': 'Văn bản trên màn hình',
        'annotated': 'Chế độ xem có chú thích',
        'orig_video': 'Video máy ảnh gốc',
        'proc_video': 'Video camera đã qua xử lý',
        'orig_photo': 'Ảnh máy ảnh gốc',
        'proc_photo': 'Ảnh camera đã qua xử lý',
        'pair_video': 'Video gốc và đã qua xử lý',
        'pair_photo': 'Ảnh gốc và đã qua xử lý',
        'open_orig': 'Mở video gốc',
        'open_proc': 'Mở video đã xử lý',
        'orig_view': 'Chế độ xem camera gốc',
        'proc_view': 'Chế độ xem camera đã xử lý',
        'analyzed': 'Xem trợ lý được phân tích',
        'captured_proc': 'Chế độ xem camera đã xử lý đã chụp'},
 'th': {'title': 'OkuFlow บันทึกการบรรยาย',
        'started': 'เริ่มต้น %1',
        'conversation': 'การสนทนา',
        'expand': 'ขยายทั้งหมด',
        'collapse': 'ยุบทั้งหมด',
        'theme': 'สีอ่อนหรือสีเข้ม',
        'video': 'บันทึกวิดีโอแล้ว',
        'photo': 'จับภาพแล้ว',
        'explain': 'คำอธิบายฉาก',
        'read': 'ข้อความบนหน้าจอ',
        'annotated': 'มุมมองที่มีคำอธิบายประกอบ',
        'orig_video': 'วิดีโอจากกล้องต้นฉบับ',
        'proc_video': 'วิดีโอกล้องที่ประมวลผลแล้ว',
        'orig_photo': 'ภาพถ่ายจากกล้องต้นฉบับ',
        'proc_photo': 'ภาพถ่ายจากกล้องที่ประมวลผลแล้ว',
        'pair_video': 'วิดีโอต้นฉบับและประมวลผล',
        'pair_photo': 'ภาพถ่ายต้นฉบับและประมวลผลแล้ว',
        'open_orig': 'เปิดวิดีโอต้นฉบับ',
        'open_proc': 'เปิดวิดีโอที่ประมวลผลแล้ว',
        'orig_view': 'มุมมองกล้องดั้งเดิม',
        'proc_view': 'มุมมองกล้องที่ประมวลผลแล้ว',
        'analyzed': 'ดูผู้ช่วยวิเคราะห์',
        'captured_proc': 'มุมมองกล้องที่ประมวลผลแล้ว'},
 'bn': {'title': 'OkuFlow লেকচার নোট',
        'started': 'শুরু হয়েছে %1',
        'conversation': 'কথোপকথন',
        'expand': 'সব প্রসারিত',
        'collapse': 'সব সঙ্কুচিত করুন',
        'theme': 'হালকা বা গাঢ় রং',
        'video': 'ভিডিও রেকর্ড করা হয়েছে',
        'photo': 'ছবি তোলা হয়েছে',
        'explain': 'দৃশ্যের ব্যাখ্যা',
        'read': 'স্ক্রিনে পাঠ্য',
        'annotated': 'টীকাযুক্ত দৃশ্য',
        'orig_video': 'অরিজিনাল ক্যামেরা ভিডিও',
        'proc_video': 'প্রক্রিয়াকৃত ক্যামেরা ভিডিও',
        'orig_photo': 'আসল ক্যামেরা ছবি',
        'proc_photo': 'প্রক্রিয়াকৃত ক্যামেরা ছবি',
        'pair_video': 'আসল এবং প্রক্রিয়াকৃত ভিডিও',
        'pair_photo': 'আসল এবং প্রক্রিয়াকৃত ছবি',
        'open_orig': 'আসল ভিডিও খুলুন',
        'open_proc': 'প্রক্রিয়াকৃত ভিডিও খুলুন',
        'orig_view': 'আসল ক্যামেরা ভিউ',
        'proc_view': 'প্রক্রিয়াকৃত ক্যামেরা ভিউ',
        'analyzed': 'সহকারী বিশ্লেষণ দেখুন',
        'captured_proc': 'ক্যাপচার করা প্রক্রিয়াকৃত ক্যামেরা ভিউ'},
 'fa': {'title': 'OkuFlow نکات سخنرانی',
        'started': 'شروع %1',
        'conversation': 'گفتگو',
        'expand': 'همه را گسترش دهید',
        'collapse': 'همه را جمع کن',
        'theme': 'رنگ های روشن یا تیره',
        'video': 'ویدئو ضبط شد',
        'photo': 'عکس گرفته شد',
        'explain': 'توضیح صحنه',
        'read': 'متن روی صفحه',
        'annotated': 'نمای مشروح',
        'orig_video': 'فیلم دوربین اصلی',
        'proc_video': 'فیلم دوربین پردازش شده',
        'orig_photo': 'عکس دوربین اصلی',
        'proc_photo': 'عکس دوربین پردازش شده',
        'pair_video': 'ویدیوی اصلی و پردازش شده',
        'pair_photo': 'عکس اصلی و پردازش شده',
        'open_orig': 'ویدیوی اصلی را باز کنید',
        'open_proc': 'ویدیوی پردازش شده را باز کنید',
        'orig_view': 'نمای دوربین اصلی',
        'proc_view': 'نمای دوربین پردازش شده',
        'analyzed': 'دستیار تجزیه و تحلیل شده را مشاهده کنید',
        'captured_proc': 'نمای دوربین پردازش شده را ضبط کرد'},
 'he': {'title': 'OkuFlow הערות הרצאה',
        'started': 'התחיל %1',
        'conversation': 'שיחה',
        'expand': 'הרחב הכל',
        'collapse': 'כווץ הכל',
        'theme': 'צבעים בהירים או כהים',
        'video': 'סרטון הוקלט',
        'photo': 'התמונה צולמה',
        'explain': 'הסבר על הסצנה',
        'read': 'טקסט על המסך',
        'annotated': 'תצוגה מוערת',
        'orig_video': 'סרטון מצלמה מקורי',
        'proc_video': 'סרטון מצלמה מעובד',
        'orig_photo': 'תמונת מצלמה מקורית',
        'proc_photo': 'תמונת מצלמה מעובדת',
        'pair_video': 'סרטון מקורי ומעובד',
        'pair_photo': 'תמונה מקורית ומעובדת',
        'open_orig': 'פתח את הסרטון המקורי',
        'open_proc': 'פתח את הסרטון המעובד',
        'orig_view': 'תצוגת מצלמה מקורית',
        'proc_view': 'תצוגת מצלמה מעובדת',
        'analyzed': 'הצג את העוזר מנותח',
        'captured_proc': 'תצוגת מצלמה מעובדת שצולמה'},
 'ur': {'title': 'OkuFlow لیکچر نوٹس',
        'started': 'شروع ہوا %1',
        'conversation': 'بات چیت',
        'expand': 'سبھی کو پھیلائیں۔',
        'collapse': 'سب کو سمیٹیں۔',
        'theme': 'ہلکے یا گہرے رنگ',
        'video': 'ویڈیو ریکارڈ کی گئی۔',
        'photo': 'تصویر کھینچی گئی۔',
        'explain': 'منظر کی وضاحت',
        'read': 'اسکرین پر متن',
        'annotated': 'تشریح شدہ منظر',
        'orig_video': 'اصل کیمرہ ویڈیو',
        'proc_video': 'پروسیس شدہ کیمرہ ویڈیو',
        'orig_photo': 'اصل کیمرے کی تصویر',
        'proc_photo': 'پروسیس شدہ کیمرے کی تصویر',
        'pair_video': 'اصل اور پروسیس شدہ ویڈیو',
        'pair_photo': 'اصل اور پروسیس شدہ تصویر',
        'open_orig': 'اصلی ویڈیو کھولیں۔',
        'open_proc': 'پروسیس شدہ ویڈیو کھولیں۔',
        'orig_view': 'اصل کیمرے کا منظر',
        'proc_view': 'پروسیسڈ کیمرا ویو',
        'analyzed': 'اسسٹنٹ کا تجزیہ دیکھیں',
        'captured_proc': 'پراسیس شدہ کیمرے کا منظر'}}

LECTURE_SPEECH = (
    "Good morning everyone. Today we will talk about the Fourier transform. "
    "A signal in time can be written as a sum of sine waves. "
    "The frequency spectrum shows how strong each sine wave is. "
    "Please open chapter four of the textbook. "
    "We will solve the first exercise together after the break."
)


def raw_pieces(path: Path, delimiter: str) -> str:
    text = path.read_text(encoding="utf-8")
    pieces = re.findall(r'R"' + delimiter + r"\((.*?)\)" + delimiter + '"', text, re.S)
    if not pieces:
        sys.exit(f"No raw string found in {path}")
    return "".join(pieces)


def esc(value: str) -> str:
    return html.escape(value, quote=True)


def section(time: str, heading: str, content: str, *, css: str = "", kind: str = "", lang: str = "en",
            open_: bool = True, preview: str = "", conversation: str = "") -> str:
    # Mirrors AssistiveRuntime::AppendNoteHtmlSection.
    classes = "note" + (f" {css}" if css else "")
    direction = "rtl" if lang in {"ar", "fa", "he", "ur"} else "ltr"
    attributes = f' class="{esc(classes)}" dir="{direction}"'
    if kind:
        attributes += f' data-kind="{kind}"'
    if conversation:
        attributes += f' data-conversation="{esc(conversation)}"'
    preview_html = f'<span class="preview">{esc(preview)}</span>'
    return (
        f'  <details{attributes} lang="{esc(lang)}"{" open" if open_ else ""}>\n'
        f'    <summary><span class="ic" aria-hidden="true"></span><h2 class="ttl">{esc(heading)}</h2>{preview_html}'
        f'<time class="at">{esc(time)}</time></summary>\n'
        f'    <div class="note-body">\n{content}    </div>\n'
        f'  </details>\n'
    )


def media_pair(s: dict, original: str, processed: str, video: bool) -> str:
    # Mirrors AssistiveRuntime::AppendNoteMediaPair.
    if video:
        def player(url: str, label: str) -> str:
            return (f'        <video controls preload="metadata">\n'
                    f'          <source src="{esc(url)}" type="video/mp4">\n'
                    f'          <a href="{esc(url)}">{esc(label)}</a>\n'
                    f'        </video>\n')
        original_media, processed_media = player(original, s["open_orig"]), player(processed, s["open_proc"])
    else:
        def image(url: str, alt: str) -> str:
            return f'        <a href="{esc(url)}"><img src="{esc(url)}" alt="{esc(alt)}" loading="lazy"></a>\n'
        original_media, processed_media = image(original, s["orig_view"]), image(processed, s["proc_view"])
    original_caption = s["orig_video"] if video else s["orig_photo"]
    processed_caption = s["proc_video"] if video else s["proc_photo"]
    content = (
        '    <div class="media-grid">\n'
        '      <figure>\n'
        f'{original_media}'
        f'        <figcaption><a href="{esc(original)}">{esc(original_caption)}</a></figcaption>\n'
        '      </figure>\n'
        '      <figure>\n'
        f'{processed_media}'
        f'        <figcaption><a href="{esc(processed)}">{esc(processed_caption)}</a></figcaption>\n'
        '      </figure>\n'
        '    </div>\n'
    )
    if video:
        content += (
            f'    <section class="transcribe-tools" data-transcribe-src="{esc(original)}">\n'
            '      <div class="transcribe-actions"><button type="button" class="transcribe-start">Transcribe</button>'
            '<button type="button" class="transcribe-stop" hidden>Stop</button>'
            '<button type="button" class="transcribe-export" hidden>Save as text</button></div>\n'
            '      <p class="transcribe-status" role="status"></p>\n'
            '      <div class="transcribe-result" hidden><h3 class="transcribe-heading">Transcript</h3>'
            '<div class="transcribe-lines"></div></div>\n'
            '    </section>\n'
        )
    return content


def ai_note(question: str, image: str, answer: str, s: dict) -> str:
    content = ""
    if question:
        content += f'    <p class="note-ask">{esc(question)}</p>\n'
    if image:
        content += ('    <figure class="ai-shot">\n'
                    f'      <a href="{esc(image)}"><img src="{esc(image)}" alt="{esc(s["analyzed"])}" loading="lazy"></a>\n'
                    '    </figure>\n')
    content += f'    <p class="note-text">{esc(answer)}</p>\n'
    return content


def run(command: list[str]) -> bool:
    try:
        subprocess.run(command, check=True, capture_output=True)
        return True
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"  skipped: {' '.join(command[:2])} ({error})")
        return False


def synthesize_speech(path: Path) -> bool:
    if os.name != "nt":
        return False
    wave_path = str(path).replace("'", "''")
    script = (
        "Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        f"$s.SetOutputToWaveFile('{wave_path}'); $s.Speak('{LECTURE_SPEECH}'); $s.Dispose()"
    )
    shell = shutil.which("pwsh")
    return bool(shell) and run([shell, "-NoProfile", "-Command", script])


def slide_filter(text: str, font: str) -> str:
    return (f"drawtext=fontfile='{font}':text='{text}':fontcolor=0x222222:fontsize=54:x=(w-text_w)/2:y=h*0.32,"
            f"drawtext=fontfile='{font}':text='X(f) = sum of sine waves':fontcolor=0x444444:fontsize=40:x=(w-text_w)/2:y=h*0.55")


def make_media(out: Path, day: str, speech: Path | None) -> dict:
    """Creates sample recordings and photos; returns paths relative to Notes/."""
    rec = out / "Recordings" / day
    pho = out / "Photos" / day
    img = out / "Notes" / "images"
    for folder in (rec, pho, img):
        folder.mkdir(parents=True, exist_ok=True)
    names = {
        "vid_o": rec / "VID_101500_original.mp4", "vid_p": rec / "VID_101500_processed.mp4",
        "pho_o": pho / "IMG_102210_original.jpg", "pho_p": pho / "IMG_102210_processed.jpg",
        "ai": img / "AI_scene.jpg", "draw": pho / "IMG_103005_annotated.png",
    }
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("ffmpeg not found: the page renders without sample media.")
    else:
        font = "C\:/Windows/Fonts/segoeui.ttf" if os.name == "nt" else "DejaVuSans.ttf"
        board = f"color=c=0xf4f1e8:s=1280x720:r=30,{slide_filter('Fourier transform', font)}"
        audio = ["-i", str(speech)] if speech else ["-f", "lavfi", "-t", "20", "-i", "anullsrc=r=48000:cl=stereo"]
        # Original: a slightly dim, soft camera view. Processed: OkuFlow-style clarity.
        dim = "eq=brightness=-0.08:contrast=0.8:saturation=0.7,gblur=sigma=1.4,noise=alls=10:allf=t"
        clear = "eq=contrast=1.5:brightness=0.02,unsharp=7:7:1.6,colorlevels=rimin=0.25:gimin=0.25:bimin=0.25"
        fmp4 = ["-movflags", "frag_keyframe+empty_moov+default_base_moof"]
        common = ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ar", "48000", "-ac", "2", "-shortest", *fmp4]
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, *audio, "-vf", dim, *common, str(names["vid_o"])])
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, *audio, "-vf", clear, *common, str(names["vid_p"])])
        still = ["-frames:v", "1", "-update", "1"]
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, "-vf", dim, *still, str(names["pho_o"])])
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, "-vf", clear, *still, str(names["pho_p"])])
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, "-vf", f"{clear},scale=960:-2", *still, str(names["ai"])])
        run([ffmpeg, "-y", "-f", "lavfi", "-i", board, "-vf",
             f"{clear},drawbox=x=300:y=200:w=680:h=110:color=0xd633d3@0.9:t=8", *still, str(names["draw"])])
    notes = out / "Notes"
    return {key: os.path.relpath(path, notes).replace(os.sep, "/") for key, path in names.items()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    default_out = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "OkuFlow" / "notes-preview"
    parser.add_argument("--out", type=Path, default=default_out)
    parser.add_argument("--lang", choices=sorted(STRINGS), default="en")
    parser.add_argument("--speech", type=Path, help="WAV file used as the recording's audio")
    args = parser.parse_args()
    s = STRINGS[args.lang]
    out = args.out.resolve()
    (out / "Notes").mkdir(parents=True, exist_ok=True)

    speech = args.speech
    if not speech:
        candidate = out / "speech.wav"
        if candidate.exists() or synthesize_speech(candidate):
            speech = candidate
    now = datetime.now()
    day = now.strftime("%Y-%m-%d")
    media = make_media(out, day, speech if speech and speech.exists() else None)

    document = raw_pieces(TEMPLATE, "HTML").replace("@@TRANSCRIBE_SCRIPT@@", raw_pieces(BRIDGE, "JS"))
    created = s["started"].replace("%1", f'<time datetime="{now.isoformat(timespec="seconds")}">{now:%Y-%m-%d %H:%M:%S}</time>')
    for token, value in {
        "@@LANG@@": args.lang, "@@DIR@@": "rtl" if args.lang in {"ar", "fa", "he", "ur"} else "ltr", "@@CONVOLABEL@@": esc(s["conversation"]), "@@TITLE@@": esc(s["title"]),
        "@@TIME@@": f"{now:%Y-%m-%d %H:%M:%S}", "@@CREATED@@": created, "@@EXPAND@@": esc(s["expand"]),
        "@@COLLAPSE@@": esc(s["collapse"]), "@@THEMELABEL@@": esc(s["theme"]),
    }.items():
        document = document.replace(token, value)

    lang = args.lang
    blocks = [
        section("10:15:00", s["video"], media_pair(s, media["vid_o"], media["vid_p"], True),
                css="media", kind="video", lang=lang, preview=s["pair_video"]),
        section("10:22:10", s["photo"], media_pair(s, media["pho_o"], media["pho_p"], False),
                css="media", kind="photo", lang=lang, preview=s["pair_photo"]),
        section("10:24:31", s["explain"], ai_note("", media["ai"],
                "A lecture slide titled \"Fourier transform\". Below it the formula reads: X(f) = sum of sine waves. "
                "The lecturer is pointing at the second line.", s),
                css="ai", kind="ai", lang=lang, open_=False,
                preview="A lecture slide titled \"Fourier transform\". Below it the formula reads…"),
        section("10:26:02", s["read"], ai_note("", media["ai"], "Fourier transform\nX(f) = sum of sine waves", s),
                css="ai", kind="ai", lang=lang, open_=False, preview="Fourier transform X(f) = sum of sine waves"),
        section("10:28:40", "What does the second line on the slide mean?", ai_note(
                "What does the second line on the slide mean?", media["ai"],
                "It says that any signal can be described as a sum of sine waves with different frequencies. "
                "X(f) tells you how much of each frequency is present.", s),
                css="ai", kind="ai", lang=lang, open_=False, conversation="thr-preview-1",
                preview="It says that any signal can be described as a sum of sine waves…"),
        section("10:29:15", "Can you give a simple example?", ai_note(
                "Can you give a simple example?", "",
                "A musical chord: three notes played together. The spectrum would show three peaks, "
                "one for each note's frequency.", s),
                css="ai", kind="ai", lang=lang, open_=False, conversation="thr-preview-1",
                preview="A musical chord: three notes played together…"),
        section("10:30:05", s["annotated"],
                f'    <figure>\n      <a href="{esc(media["draw"])}"><img src="{esc(media["draw"])}" '
                f'alt="{esc(s["captured_proc"])}" loading="lazy"></a>\n      <figcaption>{esc(s["proc_view"])}</figcaption>\n    </figure>\n',
                css="media", kind="draw", lang=lang),
    ]
    page = out / "Notes" / f"NOTES_preview_{lang}.html"
    page.write_text(document + "".join(blocks) + "</main>\n</body>\n</html>\n", encoding="utf-8")
    print(f"Preview: {page}")
    print(f"To test Transcribe, choose this folder when asked for folder access: {out}")


if __name__ == "__main__":
    main()
