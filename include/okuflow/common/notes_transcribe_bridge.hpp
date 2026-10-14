#pragma once

namespace okuflow {

// Inline in the local notes document: no server, recognition runtime or model
// runs in the Windows process. The HTTPS tool receives a File over a private
// MessageChannel and returns plain segments. Only sidecars are written here.
inline constexpr char kNotesTranscribeBridge[] = R"JS(<script>
(function () {
  'use strict';
  const labels = {
      "en": {"go":"Transcribe","folder":"Allow folder access","choose":"Choose the OkuFlow folder containing this recording. Then press Transcribe again.","ready":"Folder ready. Press Transcribe to start.","unsupported":"Open these notes in Google Chrome to transcribe recordings.","blocked":"Allow pop-ups for this notes page, then try again.","waiting":"Opening OkuFlow Transcribe…","reading":"Reading audio…","loading":"Preparing the speech model…","downloading":"Downloading the speech model…","running":"Transcribing…","queued":"Waiting for another recording…","stop":"Stop","stopped":"Stopped.","done":"Transcript ready.","saved":"Transcript saved beside the recording.","saveFailed":"Transcript ready. Folder access was lost; use Save as text.","text":"Save as text","restore":"Load saved transcript","title":"Transcript","missing":"Choose the OkuFlow folder containing this recording, then try again.","connection":"The transcription tab closed or did not respond. Try again.","empty":"No speech was found.","failed":"Transcription failed."},
)JS"
R"JS(
      "tr": {"go":"Yazıya dök","folder":"Klasöre erişim izni ver","choose":"Bu kaydın bulunduğu OkuFlow klasörünü seçin. Sonra Yazıya dök düğmesine tekrar basın.","ready":"Klasör hazır. Başlamak için Yazıya dök düğmesine basın.","unsupported":"Kayıtları yazıya dökmek için bu notları Google Chrome’da açın.","blocked":"Bu not sayfası için açılır pencerelere izin verip tekrar deneyin.","waiting":"OkuFlow Yazıya Dök açılıyor…","reading":"Ses okunuyor…","loading":"Konuşma modeli hazırlanıyor…","downloading":"Konuşma modeli indiriliyor…","running":"Yazıya dökülüyor…","queued":"Başka bir kayıt bekleniyor…","stop":"Durdur","stopped":"Durduruldu.","done":"Kayıt metni hazır.","saved":"Kayıt metni, kayıt dosyasıyla aynı klasöre kaydedildi.","saveFailed":"Kayıt metni hazır. Klasöre erişim kaybedildi; «Metin olarak kaydet» düğmesini kullanın.","text":"Metin olarak kaydet","restore":"Kaydedilen kayıt metnini yükle","title":"Kayıt metni","missing":"Bu kaydın bulunduğu OkuFlow klasörünü seçip tekrar deneyin.","connection":"Yazıya dökme sekmesi kapandı veya yanıt vermedi. Tekrar deneyin.","empty":"Konuşma bulunamadı.","failed":"Kayıt yazıya dökülemedi."},
)JS"
R"JS(
      "de": {"go":"Transkribieren","folder":"Ordnerzugriff erlauben","choose":"Wählen Sie den OkuFlow-Ordner mit dieser Aufnahme. Drücken Sie danach erneut Transkribieren.","ready":"Ordner bereit. Drücken Sie Transkribieren zum Starten.","unsupported":"Öffnen Sie diese Notizen in Google Chrome, um Aufnahmen zu transkribieren.","blocked":"Erlauben Sie Pop-ups für diese Notizenseite und versuchen Sie es erneut.","waiting":"OkuFlow Transkription wird geöffnet…","reading":"Audio wird eingelesen…","loading":"Sprachmodell wird vorbereitet…","downloading":"Sprachmodell wird heruntergeladen…","running":"Transkription läuft…","queued":"Warten auf eine andere Aufnahme…","stop":"Stopp","stopped":"Gestoppt.","done":"Transkript bereit.","saved":"Transkript neben der Aufnahme gespeichert.","saveFailed":"Transkript bereit. Der Ordnerzugriff wurde entzogen; verwenden Sie „Als Text speichern“.","text":"Als Text speichern","restore":"Gespeichertes Transkript laden","title":"Transkript","missing":"Wählen Sie den OkuFlow-Ordner mit dieser Aufnahme und versuchen Sie es erneut.","connection":"Die Transkriptionsseite wurde geschlossen oder antwortet nicht. Versuchen Sie es erneut.","empty":"Keine Sprache gefunden.","failed":"Transkription fehlgeschlagen."},
)JS"
R"JS(
      "fr": {"go":"Transcrire","folder":"Autoriser l’accès au dossier","choose":"Choisissez le dossier OkuFlow contenant cet enregistrement. Appuyez ensuite à nouveau sur Transcrire.","ready":"Dossier prêt. Appuyez sur Transcrire pour commencer.","unsupported":"Ouvrez ces notes dans Google Chrome pour transcrire les enregistrements.","blocked":"Autorisez les fenêtres contextuelles pour cette page de notes, puis réessayez.","waiting":"Ouverture de l’outil de transcription OkuFlow…","reading":"Lecture audio…","loading":"Préparation du modèle de reconnaissance vocale…","downloading":"Téléchargement du modèle de parole…","running":"Transcription…","queued":"En attendant un autre enregistrement…","stop":"Arrêter","stopped":"Arrêté.","done":"Transcription prête.","saved":"Transcription enregistrée à côté de l'enregistrement.","saveFailed":"Transcription prête. L'accès au dossier a été perdu ; utilisez « Enregistrer au format texte ».","text":"Enregistrer au format texte","restore":"Charger la transcription enregistrée","title":"Transcription","missing":"Choisissez le dossier OkuFlow contenant cet enregistrement, puis réessayez.","connection":"L’onglet de transcription s’est fermé ou n’a pas répondu. Réessayez.","empty":"Aucune parole détectée.","failed":"La transcription a échoué."},
)JS"
R"JS(
      "es": {"go":"Transcribir","folder":"Permitir acceso a carpeta","choose":"Seleccione la carpeta de OkuFlow que contiene esta grabación y vuelva a pulsar «Transcribir».","ready":"Carpeta lista. Presione Transcribir para comenzar.","unsupported":"Abra estas notas en Google Chrome para transcribir grabaciones.","blocked":"Permita ventanas emergentes para esta página de notas y vuelva a intentarlo.","waiting":"Abriendo OkuFlow Transcribir…","reading":"Leyendo audio…","loading":"Preparando el modelo de voz…","downloading":"Descargando el modelo de voz…","running":"Transcribiendo…","queued":"Esperando otra grabación...","stop":"Detener","stopped":"Detenido.","done":"Transcripción lista.","saved":"Transcripción guardada al lado de la grabación.","saveFailed":"Transcripción lista. Se perdió el acceso a la carpeta; utilice Guardar como texto.","text":"Guardar como texto","restore":"Cargar transcripción guardada","title":"Transcripción","missing":"Elija la carpeta OkuFlow que contiene esta grabación y vuelva a intentarlo.","connection":"La pestaña de transcripción se cerró o no respondió. Vuelva a intentarlo.","empty":"No se detectó voz.","failed":"La transcripción falló."},
)JS"
R"JS(
      "it": {"go":"Trascrivi","folder":"Consenti l'accesso alla cartella","choose":"Scegli la cartella OkuFlow con questa registrazione. Poi premi di nuovo Trascrivi.","ready":"Cartella pronta. Premi Trascrivi per iniziare.","unsupported":"Apri queste note in Google Chrome per trascrivere le registrazioni.","blocked":"Consenti i popup per questa pagina delle note, quindi riprova.","waiting":"Apertura di OkuFlow Trascrizione…","reading":"Lettura dell'audio…","loading":"Preparazione del modello vocale…","downloading":"Download del modello vocale…","running":"Trascrizione…","queued":"In attesa di un'altra registrazione...","stop":"Interrompi","stopped":"Operazione interrotta.","done":"Trascrizione pronta.","saved":"Trascrizione salvata accanto alla registrazione.","saveFailed":"Trascrizione pronta. L'accesso alla cartella è stato perso; utilizzare Salva come testo.","text":"Salva come testo","restore":"Carica la trascrizione salvata","title":"Trascrizione","missing":"Scegli la cartella OkuFlow contenente questa registrazione, quindi riprova.","connection":"La scheda di trascrizione è stata chiusa o non ha risposto. Riprova.","empty":"Non è stato rilevato parlato.","failed":"La trascrizione non è riuscita."},
)JS"
R"JS(
      "pt-BR": {"go":"Transcrever","folder":"Permitir acesso à pasta","choose":"Escolha a pasta OkuFlow que contém esta gravação. Em seguida, pressione Transcrever novamente.","ready":"Pasta pronta. Pressione Transcrever para começar.","unsupported":"Abra estas notas no Google Chrome para transcrever as gravações.","blocked":"Permita pop-ups para esta página de notas e tente novamente.","waiting":"Abrindo OkuFlow Transcrever…","reading":"Lendo áudio…","loading":"Preparando o modelo de fala…","downloading":"Baixando o modelo de fala…","running":"Transcrevendo…","queued":"Aguardando outra gravação…","stop":"Parar","stopped":"Parado.","done":"Transcrição pronta.","saved":"Transcrição salva na mesma pasta da gravação.","saveFailed":"Transcrição pronta. O acesso à pasta foi perdido; use Salvar como texto.","text":"Salvar como texto","restore":"Carregar transcrição salva","title":"Transcrição","missing":"Escolha a pasta OkuFlow que contém esta gravação e tente novamente.","connection":"A guia de transcrição fechou ou não respondeu. Tente novamente.","empty":"Nenhuma fala encontrada.","failed":"Falha na transcrição."},
)JS"
R"JS(
      "ru": {"go":"Расшифровать","folder":"Разрешить доступ к папке","choose":"Выберите папку OkuFlow, в которой находится эта запись. Затем снова нажмите «Расшифровать».","ready":"Папка выбрана. Нажмите «Расшифровать», чтобы начать.","unsupported":"Откройте эти заметки в Google Chrome, чтобы расшифровать записи.","blocked":"Разрешите всплывающие окна для этой страницы заметок и повторите попытку.","waiting":"Открывается инструмент расшифровки OkuFlow…","reading":"Чтение аудио…","loading":"Подготовка речевой модели…","downloading":"Загрузка модели речи…","running":"Расшифровка…","queued":"Ожидание следующей записи…","stop":"Стоп","stopped":"Остановлено.","done":"Расшифровка готова.","saved":"Расшифровка сохранена в той же папке, что и запись.","saveFailed":"Расшифровка готова. Доступ к папке потерян; выберите «Сохранить как текст».","text":"Сохранить как текст","restore":"Загрузить сохраненную расшифровку","title":"Расшифровка","missing":"Выберите папку OkuFlow, содержащую эту запись, затем повторите попытку.","connection":"Вкладка транскрипции закрылась или не отвечает. Попробуйте еще раз.","empty":"Речь не найдена.","failed":"Транскрипция не удалась."},
)JS"
R"JS(
      "uk": {"go":"Транскрибувати","folder":"Дозволити доступ до папки","choose":"Виберіть папку OkuFlow, яка містить цей запис. Потім знову натисніть «Транскрибувати».","ready":"Папка готова. Натисніть Транскрибувати, щоб почати.","unsupported":"Відкрийте ці нотатки в Google Chrome, щоб транскрибувати записи.","blocked":"Дозвольте спливаючі вікна для цієї сторінки нотаток і повторіть спробу.","waiting":"Відкриття OkuFlow Транскрибувати…","reading":"Читання аудіо…","loading":"Готуємо модель мовлення…","downloading":"Завантаження моделі мовлення…","running":"Транскрибування…","queued":"Чекаємо ще один запис…","stop":"Стоп","stopped":"Зупинено.","done":"Транскрипт готовий.","saved":"Транскрипт збережено в тій самій папці, що й запис.","saveFailed":"Транскрипт готовий, але доступ до папки втрачено. Скористайтеся командою «Зберегти як текст».","text":"Зберегти як текст","restore":"Відкрити збережений транскрипт","title":"Транскрипт","missing":"Виберіть папку OkuFlow, що містить цей запис, і повторіть спробу.","connection":"Вкладка транскрипції закрита або не відповідає. Спробуйте знову.","empty":"Мовлення не знайдено.","failed":"Помилка транскрипції."},
)JS"
R"JS(
      "ar": {"go":"تفريغ التسجيل","folder":"السماح بالوصول إلى المجلد","choose":"اختر مجلد OkuFlow الذي يحتوي على هذا التسجيل، ثم اضغط «تفريغ التسجيل» مجددًا.","ready":"المجلد جاهز. اضغط «تفريغ التسجيل» للبدء.","unsupported":"افتح هذه الملاحظات في Google Chrome لتفريغ التسجيلات نصيًا.","blocked":"اسمح بالنوافذ المنبثقة لصفحة الملاحظات هذه، ثم حاول مرة أخرى.","waiting":"جارٍ فتح OkuFlow للتفريغ النصي…","reading":"قراءة الصوت…","loading":"جارٍ تجهيز نموذج التعرّف على الكلام","downloading":"جارٍ تنزيل نموذج التعرّف على الكلام","running":"جارٍ تفريغ التسجيل…","queued":"في انتظار تسجيل آخر...","stop":"إيقاف","stopped":"توقف.","done":"أصبح التفريغ جاهزًا.","saved":"حُفظ النص المفرّغ في المجلد نفسه الذي يحتوي على ملف التسجيل.","saveFailed":"أصبح التفريغ جاهزًا، لكن تعذّر الوصول إلى المجلد. استخدم «حفظ كنص».","text":"حفظ كنص","restore":"استعادة التفريغ المحفوظ","title":"تفريغ نصي","missing":"اختر المجلد OkuFlow الذي يحتوي على هذا التسجيل ثم حاول مرة أخرى.","connection":"أُغلقت علامة تبويب التفريغ أو لم تستجب. حاول مجددًا.","empty":"لم يُعثر على كلام في التسجيل.","failed":"تعذّر تفريغ التسجيل."},
)JS"
R"JS(
      "hi": {"go":"ट्रांसक्राइब करें","folder":"फ़ोल्डर पहुंच की अनुमति दें","choose":"इस रिकॉर्डिंग वाले OkuFlow फ़ोल्डर को चुनें। फिर दोबारा ट्रांसक्राइब दबाएँ।","ready":"फ़ोल्डर तैयार है। शुरू करने के लिए «ट्रांसक्राइब करें» दबाएँ.","unsupported":"रिकॉर्डिंग को ट्रांसक्राइब करने के लिए इन नोट्स को Google Chrome में खोलें।","blocked":"इस नोट्स पृष्ठ के लिए पॉप-अप की अनुमति दें, फिर पुनः प्रयास करें।","waiting":"OkuFlow Transcribe खुल रहा है…","reading":"ऑडियो पढ़ा जा रहा है...","loading":"स्पीच मॉडल तैयार किया जा रहा है...","downloading":"भाषण मॉडल डाउनलोड हो रहा है...","running":"ट्रांसक्राइब हो रहा है","queued":"अन्य रिकॉर्डिंग की प्रतीक्षा में...","stop":"रुकें","stopped":"रुक गया.","done":"प्रतिलेख तैयार.","saved":"ट्रांसक्रिप्ट रिकॉर्डिंग वाले फ़ोल्डर में सहेजा गया।","saveFailed":"ट्रांसक्रिप्ट तैयार है। फ़ोल्डर का ऐक्सेस नहीं रहा; «पाठ के रूप में सहेजें» चुनें.","text":"पाठ के रूप में सहेजें","restore":"सहेजा गया प्रतिलेख लोड करें","title":"प्रतिलेख","missing":"इस रिकॉर्डिंग वाले OkuFlow फ़ोल्डर को चुनें, फिर पुनः प्रयास करें।","connection":"ट्रांसक्रिप्शन टैब बंद हो गया या जवाब नहीं दे रहा है। फिर कोशिश करें।","empty":"कोई आवाज़ नहीं मिली।","failed":"प्रतिलेखन विफल रहा."},
)JS"
R"JS(
      "ja": {"go":"文字起こし","folder":"フォルダーへのアクセスを許可する","choose":"この録音がある OkuFlow フォルダーを選択してから、もう一度「文字起こし」を押してください。","ready":"フォルダーの準備ができました。「文字起こし」を押して開始してください。","unsupported":"Google Chrome でこれらのメモを開いて、録音を文字に起こします。","blocked":"このノート ページのポップアップを許可してから、もう一度お試しください。","waiting":"OkuFlow 文字起こしを開いています…","reading":"音声を読み込んでいます…","loading":"音声モデルを準備しています…","downloading":"音声モデルをダウンロードしています…","running":"文字起こし中…","queued":"別の録音を待っています…","stop":"停止","stopped":"停止しました。","done":"文字起こしの準備ができました。","saved":"文字起こしを録音ファイルと同じフォルダーに保存しました。","saveFailed":"トランスクリプトの準備ができました。フォルダーへのアクセスが失われました。 「テキストとして保存」を使用します。","text":"テキストとして保存","restore":"保存されたトランスクリプトをロードする","title":"文字起こし","missing":"この録音が含まれている OkuFlow フォルダーを選択して、もう一度試してください。","connection":"文字起こしタブが閉じたか、応答しませんでした。もう一度やり直してください。","empty":"音声が見つかりませんでした。","failed":"文字起こしに失敗しました。"},
)JS"
R"JS(
      "ko": {"go":"전사","folder":"폴더 액세스 허용","choose":"이 녹음이 있는 OkuFlow 폴더를 선택한 다음 «전사»를 다시 누르세요.","ready":"폴더가 준비되었습니다. 시작하려면 전사를 누르세요.","unsupported":"녹음을 전사하려면 Google Chrome에서 이 노트를 여세요.","blocked":"이 노트 페이지에 대한 팝업을 허용한 후 다시 시도하세요.","waiting":"OkuFlow 전사 여는 중…","reading":"오디오 읽는 중…","loading":"음성 모델 준비 중…","downloading":"음성 모델 다운로드 중…","running":"전사 중…","queued":"다른 녹음을 기다리는 중…","stop":"중지","stopped":"중지되었습니다.","done":"전사문이 준비되었습니다.","saved":"녹음 파일과 같은 폴더에 전사문이 저장되었습니다.","saveFailed":"전사문이 준비되었습니다. 폴더 접근 권한이 없어졌습니다. «텍스트로 저장»을 사용하세요.","text":"텍스트로 저장","restore":"저장된 전사본 불러오기","title":"전사문","missing":"이 녹음이 포함된 OkuFlow 폴더를 선택한 후 다시 시도하세요.","connection":"전사 탭이 닫혔거나 응답하지 않았습니다. 다시 시도해 보세요.","empty":"음성을 찾을 수 없습니다.","failed":"전사에 실패했습니다."},
)JS"
R"JS(
      "zh-Hans": {"go":"转录","folder":"允许文件夹访问","choose":"选择包含此录音的OkuFlow文件夹。然后再次按转录。","ready":"文件夹准备好了。按转录开始。","unsupported":"打开Google Chrome中的这些笔记来转录录音。","blocked":"允许此注释页面弹出窗口，然后重试。","waiting":"正在打开 OkuFlow 转录…","reading":"正在读取音频...","loading":"准备语音模型...","downloading":"正在下载语音模型...","running":"正在转录…","queued":"等待另一次录音……","stop":"停止","stopped":"已停止。","done":"转录文本已准备好。","saved":"文字记录保存在录音旁边。","saveFailed":"成绩单准备好了。文件夹访问权限丢失；使用另存为文本。","text":"另存为文本","restore":"加载已保存的转录文本","title":"转录文本","missing":"选择包含此录音的OkuFlow文件夹，然后重试。","connection":"转录选项卡关闭或没有响应。再试一次。","empty":"未检测到语音。","failed":"转录失败。"},
)JS"
R"JS(
      "nl": {"go":"Transcriberen","folder":"Maptoegang toestaan","choose":"Kies de map OkuFlow die deze opname bevat. Druk vervolgens nogmaals op Transcriberen.","ready":"Map klaar. Druk op Transcriberen om te beginnen.","unsupported":"Open deze notities in Google Chrome om opnames te transcriberen.","blocked":"Sta pop-ups toe voor deze notitiepagina en probeer het opnieuw.","waiting":"OkuFlow Transcriberen openen…","reading":"Audio lezen...","loading":"Het spraakmodel voorbereiden…","downloading":"Het spraakmodel downloaden…","running":"Transcriberen…","queued":"Wachten op een nieuwe opname...","stop":"Stop","stopped":"Gestopt.","done":"Transcript klaar.","saved":"Transcript opgeslagen naast de opname.","saveFailed":"Transcript klaar. Toegang tot de map is verloren; gebruik Opslaan als tekst.","text":"Opslaan als tekst","restore":"Laad het opgeslagen transcript","title":"Transcript","missing":"Kies de map OkuFlow die deze opname bevat en probeer het opnieuw.","connection":"Het transcriptietabblad is gesloten of heeft niet gereageerd. Probeer het opnieuw.","empty":"Er is geen spraak gevonden.","failed":"Transcriptie is mislukt."},
)JS"
R"JS(
      "pl": {"go":"Transkrybuj","folder":"Zezwól na dostęp do folderu","choose":"Wybierz folder OkuFlow zawierający to nagranie. Następnie ponownie naciśnij opcję Transkrypcja.","ready":"Folder gotowy. Naciśnij Transkrypcja, aby rozpocząć.","unsupported":"Otwórz te notatki w Google Chrome, aby dokonać transkrypcji nagrań.","blocked":"Zezwól na wyskakujące okienka na tej stronie notatek, a następnie spróbuj ponownie.","waiting":"Otwieranie OkuFlow Transkrypcja…","reading":"Czytanie dźwięku…","loading":"Przygotowanie modelu mowy…","downloading":"Pobieram model mowy…","running":"Transkrypcja…","queued":"Czekam na kolejne nagranie…","stop":"Zatrzymaj","stopped":"Zatrzymany.","done":"Transkrypcja gotowa.","saved":"Transkrypcja zapisana obok nagrania.","saveFailed":"Transkrypcja gotowa. Utracono dostęp do folderu; użyj Zapisz jako tekst.","text":"Zapisz jako tekst","restore":"Załaduj zapisaną transkrypcję","title":"Transkrypcja","missing":"Wybierz folder OkuFlow zawierający to nagranie, a następnie spróbuj ponownie.","connection":"Karta transkrypcji została zamknięta lub nie odpowiedziała. Spróbuj ponownie.","empty":"Nie znaleziono mowy.","failed":"Transkrypcja nie powiodła się."},
)JS"
R"JS(
      "cs": {"go":"Přepsat","folder":"Povolit přístup ke složce","choose":"Vyberte složku OkuFlow s touto nahrávkou. Poté znovu stiskněte «Přepsat».","ready":"Složka připravena. Začněte stisknutím «Přepsat».","unsupported":"Otevřete tyto poznámky v Google Chrome pro přepis nahrávek.","blocked":"Povolte vyskakovací okna pro tuto stránku poznámek a zkuste to znovu.","waiting":"Otevírá se OkuFlow Přepis…","reading":"Čtení zvuku…","loading":"Příprava řečového modelu…","downloading":"Stahování modelu řeči…","running":"Přepis…","queued":"Čekání na další záznam…","stop":"Zastavit","stopped":"Zastaveno.","done":"Přepis připraven.","saved":"Přepis uložen vedle nahrávky.","saveFailed":"Přepis připraven. Přístup ke složce byl ztracen; použijte Uložit jako text.","text":"Uložit jako text","restore":"Načíst uložený přepis","title":"Přepis","missing":"Vyberte složku OkuFlow obsahující tuto nahrávku a zkuste to znovu.","connection":"Karta přepisu se zavřela nebo neodpověděla. Zkuste to znovu.","empty":"Nebyla nalezena žádná řeč.","failed":"Přepis se nezdařil."},
)JS"
R"JS(
      "sv": {"go":"Transkribera","folder":"Tillåt mappåtkomst","choose":"Välj mappen OkuFlow som innehåller denna inspelning. Tryck sedan på Transkribera igen.","ready":"Mapp klar. Tryck på Transkribera för att starta.","unsupported":"Öppna dessa anteckningar i Google Chrome för att transkribera inspelningar.","blocked":"Tillåt popup-fönster för den här anteckningssidan och försök sedan igen.","waiting":"Öppnar OkuFlow Transkribera...","reading":"Läser ljud...","loading":"Förbereder talmodellen...","downloading":"Laddar ned talmodellen...","running":"Transkriberar...","queued":"Väntar på en annan inspelning...","stop":"Stopp","stopped":"Slutade.","done":"Avskriften är klar.","saved":"Avskriften har sparats bredvid inspelningen.","saveFailed":"Avskriften är klar. Mappåtkomst gick förlorad; använd Spara som text.","text":"Spara som text","restore":"Ladda sparad avskrift","title":"Transkription","missing":"Välj mappen OkuFlow som innehåller denna inspelning och försök sedan igen.","connection":"Transkriptionsfliken stängdes eller svarade inte. Försök igen.","empty":"Inget tal hittades.","failed":"Transkriptionen misslyckades."},
)JS"
R"JS(
      "id": {"go":"Transkripsikan","folder":"Izinkan akses folder","choose":"Pilih folder OkuFlow yang berisi rekaman ini. Lalu tekan Transkrip lagi.","ready":"Folder sudah siap. Tekan Transkripsikan untuk memulai.","unsupported":"Buka catatan ini di Google Chrome untuk mentranskripsikan rekaman.","blocked":"Izinkan pop-up untuk halaman catatan ini, lalu coba lagi.","waiting":"Membuka OkuFlow Transkrip…","reading":"Membaca audio…","loading":"Mempersiapkan model ucapan…","downloading":"Mengunduh model ucapan…","running":"Mentranskripsikan…","queued":"Menunggu rekaman lainnya…","stop":"Berhenti","stopped":"Berhenti.","done":"Transkrip sudah siap.","saved":"Transkrip disimpan di samping rekaman.","saveFailed":"Transkrip sudah siap. Akses folder hilang; gunakan Simpan sebagai teks.","text":"Simpan sebagai teks","restore":"Muat transkrip yang disimpan","title":"Transkrip","missing":"Pilih folder OkuFlow yang berisi rekaman ini, lalu coba lagi.","connection":"Tab transkripsi ditutup atau tidak merespons. Coba lagi.","empty":"Tidak ada ucapan yang ditemukan.","failed":"Transkripsi gagal."},
)JS"
R"JS(
      "vi": {"go":"Phiên âm","folder":"Cho phép truy cập thư mục","choose":"Chọn thư mục OkuFlow chứa bản ghi này. Sau đó nhấn Transcribe một lần nữa.","ready":"Thư mục đã sẵn sàng. Nhấn Phiên âm để bắt đầu.","unsupported":"Mở các ghi chú này trong Google Chrome để chuyển bản ghi âm thành văn bản.","blocked":"Cho phép cửa sổ bật lên cho trang ghi chú này, sau đó thử lại.","waiting":"Đang mở OkuFlow Phiên âm…","reading":"Đang đọc âm thanh…","loading":"Đang chuẩn bị mô hình giọng nói…","downloading":"Đang tải xuống mẫu giọng nói…","running":"Phiên âm…","queued":"Đang chờ bản ghi âm khác…","stop":"Dừng lại","stopped":"Đã dừng lại.","done":"Bản chép lời đã sẵn sàng.","saved":"Bản chép lời đã được lưu trong cùng thư mục với bản ghi.","saveFailed":"Bản chép lời đã sẵn sàng. Quyền truy cập thư mục đã mất; hãy dùng Lưu dưới dạng văn bản.","text":"Lưu dưới dạng văn bản","restore":"Tải bản chép lời đã lưu","title":"Bản chép lời","missing":"Chọn thư mục OkuFlow chứa bản ghi này rồi thử lại.","connection":"Tab phiên âm đã đóng hoặc không phản hồi. Hãy thử lại.","empty":"Không phát hiện lời nói.","failed":"Phiên âm không thành công."},
)JS"
R"JS(
      "th": {"go":"ถอดเสียง","folder":"อนุญาตให้เข้าถึงโฟลเดอร์","choose":"เลือกโฟลเดอร์ OkuFlow ที่มีการบันทึกนี้ จากนั้นกดถอดเสียงอีกครั้ง","ready":"โฟลเดอร์พร้อมแล้ว กด ถอดเสียง เพื่อเริ่มต้น","unsupported":"เปิดบันทึกเหล่านี้ใน Google Chrome เพื่อถอดเสียงการบันทึก","blocked":"อนุญาตป๊อปอัปสำหรับหน้าบันทึกย่อนี้ จากนั้นลองอีกครั้ง","waiting":"กำลังเปิด OkuFlow ถอดเสียง...","reading":"กำลังอ่านเสียง...","loading":"กำลังเตรียมโมเดลคำพูด...","downloading":"กำลังดาวน์โหลดโมเดลคำพูด...","running":"กำลังถอดเสียง...","queued":"กำลังรอบันทึกอื่น…","stop":"หยุด","stopped":"หยุดแล้ว","done":"บทถอดเสียงพร้อมแล้ว","saved":"การถอดเสียงที่บันทึกไว้ข้างการบันทึก","saveFailed":"บทถอดเสียงพร้อมแล้ว การเข้าถึงโฟลเดอร์สูญหาย ใช้บันทึกเป็นข้อความ","text":"บันทึกเป็นข้อความ","restore":"โหลดการถอดเสียงที่บันทึกไว้","title":"การถอดเสียง","missing":"เลือกโฟลเดอร์ OkuFlow ที่มีการบันทึกนี้ แล้วลองอีกครั้ง","connection":"แท็บการถอดเสียงปิดหรือไม่ตอบสนอง ลองอีกครั้ง","empty":"ไม่พบคำพูด","failed":"การถอดเสียงล้มเหลว"},
)JS"
R"JS(
      "bn": {"go":"ট্রান্সক্রাইব করুন","folder":"ফোল্ডার অ্যাক্সেসের অনুমতি দিন","choose":"এই রেকর্ডিং ধারণকারী OkuFlow ফোল্ডারটি চয়ন করুন৷ তারপর আবার ট্রান্সক্রাইব টিপুন।","ready":"ফোল্ডার প্রস্তুত. শুরু করতে ট্রান্সক্রাইব টিপুন।","unsupported":"রেকর্ডিং ট্রান্সক্রাইব করতে এই নোটগুলি Google Chrome-এ খুলুন।","blocked":"এই নোট পৃষ্ঠার জন্য পপ-আপের অনুমতি দিন, তারপর আবার চেষ্টা করুন।","waiting":"OkuFlow ট্রান্সক্রিপ্ট খোলা হচ্ছে...","reading":"অডিও প্রক্রিয়া করা হচ্ছে...","loading":"স্পিচ মডেল প্রস্তুত করা হচ্ছে...","downloading":"স্পিচ মডেল ডাউনলোড করা হচ্ছে...","running":"ট্রান্সক্রাইব করা হচ্ছে...","queued":"অন্য রেকর্ডিংয়ের জন্য অপেক্ষা করা হচ্ছে...","stop":"থামুন","stopped":"থেমে গেল।","done":"প্রতিলিপি প্রস্তুত.","saved":"ট্রান্সক্রিপ্টটি রেকর্ডিংয়ের একই ফোল্ডারে সংরক্ষণ করা হয়েছে।","saveFailed":"ট্রান্সক্রিপ্ট প্রস্তুত। ফোল্ডারে প্রবেশাধিকার হারিয়ে গেছে; «পাঠ্য হিসাবে সংরক্ষণ করুন» ব্যবহার করুন।","text":"পাঠ্য হিসাবে সংরক্ষণ করুন","restore":"সংরক্ষিত ট্রান্সক্রিপ্ট লোড করুন","title":"ট্রান্সক্রিপ্ট","missing":"এই রেকর্ডিং ধারণকারী OkuFlow ফোল্ডারটি বেছে নিন, তারপর আবার চেষ্টা করুন।","connection":"ট্রান্সক্রিপশন ট্যাবটি বন্ধ বা সাড়া দেয়নি। আবার চেষ্টা করুন","empty":"কোনো কণ্ঠস্বর পাওয়া যায়নি।","failed":"ট্রান্সক্রিপশন ব্যর্থ হয়েছে৷"},
)JS"
R"JS(
      "fa": {"go":"رونویسی کنید","folder":"اجازه دسترسی به پوشه","choose":"پوشهٔ OkuFlow حاوی این ضبط را انتخاب کنید. سپس دوباره «رونویسی» را فشار دهید.","ready":"پوشه آماده است. برای شروع، رونویسی را فشار دهید.","unsupported":"برای رونویسی موارد ضبط شده، این یادداشت ها را در Google Chrome باز کنید.","blocked":"به پنجره‌های بازشو برای این صفحه یادداشت‌ها اجازه دهید، سپس دوباره امتحان کنید.","waiting":"باز کردن OkuFlow رونویسی…","reading":"خواندن صوت…","loading":"در حال آماده سازی مدل گفتار…","downloading":"در حال دانلود مدل گفتار…","running":"در حال رونویسی…","queued":"در انتظار ضبط دیگری…","stop":"توقف کنید","stopped":"متوقف شد.","done":"رونوشت آماده است.","saved":"رونوشت در کنار ضبط ذخیره شد.","saveFailed":"رونوشت آماده است. دسترسی به پوشه از بین رفت. از ذخیره به عنوان متن استفاده کنید.","text":"ذخیره به عنوان متن","restore":"بارگیری رونوشت ذخیره شده","title":"رونوشت","missing":"پوشه OkuFlow حاوی این ضبط را انتخاب کنید، سپس دوباره امتحان کنید.","connection":"برگه رونویسی بسته شد یا پاسخ نداد. دوباره امتحان کنید.","empty":"گفتاری پیدا نشد","failed":"رونویسی انجام نشد."},
)JS"
R"JS(
      "he": {"go":"תמלול","folder":"אפשר גישה לתיקיות","choose":"בחר בתיקייה OkuFlow המכילה את ההקלטה הזו. לאחר מכן לחץ שוב על תמלול.","ready":"התיקייה מוכנה. לחץ על תמלול כדי להתחיל.","unsupported":"פתח את ההערות האלה ב-Google Chrome כדי לתמלל הקלטות.","blocked":"אפשר חלונות קופצים עבור דף ההערות הזה, ולאחר מכן נסה שוב.","waiting":"פותח את OkuFlow תמלול...","reading":"קורא אודיו...","loading":"מכינים את מודל הדיבור...","downloading":"מוריד את מודל הדיבור...","running":"מתמלל...","queued":"מחכה להקלטה נוספת...","stop":"עצור","stopped":"נעצר.","done":"תמלול מוכן.","saved":"התמליל נשמר לצד ההקלטה.","saveFailed":"תמלול מוכן. הגישה לתיקייה אבדה; השתמש בשמירה כטקסט.","text":"שמור כטקסט","restore":"טען תמליל שמור","title":"תמלול","missing":"בחר בתיקייה OkuFlow המכילה את ההקלטה הזו, ולאחר מכן נסה שוב.","connection":"לשונית התמלול נסגרה או לא הגיבה. נסה שוב.","empty":"לא זוהה דיבור.","failed":"התמלול נכשל."},
)JS"
R"JS(
      "ur": {"go":"ٹرانسکرائب کریں","folder":"فولڈر تک رسائی کی اجازت دیں۔","choose":"اس ریکارڈنگ پر مشتمل OkuFlow فولڈر کا انتخاب کریں۔ پھر ٹرانسکرائب کو دوبارہ دبائیں۔","ready":"فولڈر تیار ہے۔ شروع کرنے کے لیے ٹرانسکرائب کو دبائیں۔","unsupported":"ریکارڈنگ کو ٹرانسکرائب کرنے کے لیے ان نوٹوں کو Google Chrome میں کھولیں۔","blocked":"اس نوٹس کے صفحہ کے لیے پاپ اپس کی اجازت دیں، پھر دوبارہ کوشش کریں۔","waiting":"OkuFlow ٹرانسکرائب کھل رہا ہے…","reading":"آڈیو پڑھ رہا ہے…","loading":"اسپیچ ماڈل تیار ہو رہا ہے…","downloading":"تقریر کا ماڈل ڈاؤن لوڈ ہو رہا ہے…","running":"ٹرانسکرائب ہو رہا ہے…","queued":"دوسری ریکارڈنگ کا انتظار ہے…","stop":"رکیں","stopped":"رک گیا۔","done":"ٹرانسکرپٹ تیار ہے۔","saved":"ٹرانسکرپٹ ریکارڈنگ کے ساتھ محفوظ ہے۔","saveFailed":"ٹرانسکرپٹ تیار ہے۔ فولڈر تک رسائی ختم ہوگئی۔ محفوظ کریں بطور ٹیکسٹ استعمال کریں۔","text":"متن کے طور پر محفوظ کریں۔","restore":"محفوظ شدہ ٹرانسکرپٹ لوڈ کریں","title":"ٹرانسکرپٹ","missing":"اس ریکارڈنگ پر مشتمل OkuFlow فولڈر کا انتخاب کریں، پھر دوبارہ کوشش کریں۔","connection":"ٹرانسکرپشن ٹیب بند ہو گیا یا جواب نہیں دیا۔ دوبارہ کوشش کریں۔","empty":"کوئی آواز نہیں ملی۔","failed":"ٹرانسکرپشن ناکام ہو گئی۔"}
  };
  const lang = document.documentElement.lang || 'en';
  const l = labels[lang] || labels[lang.split('-')[0]] || labels.en;
  const toolUrl = 'https://okuflow.com/transcribe/?ui=' + encodeURIComponent(lang);
  const toolOrigin = new URL(toolUrl).origin;
  let root = null;
  let popup = null;
  function database() {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open('okuflow-notes-files', 1);
      request.onupgradeneeded = () => request.result.createObjectStore('handles');
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  }
  async function remember(handle) {
    try {
      const db = await database();
      await new Promise((resolve, reject) => {
        const tx = db.transaction('handles', 'readwrite');
        tx.objectStore('handles').put(handle, 'root');
        tx.oncomplete = resolve; tx.onerror = () => reject(tx.error);
      });
      db.close();
    } catch (_) { /* Folder access remains usable for this page. */ }
  }
  async function recall() {
    try {
      const db = await database();
      root = await new Promise((resolve, reject) => {
        const request = db.transaction('handles').objectStore('handles').get('root');
        request.onsuccess = () => resolve(request.result || null);
        request.onerror = () => reject(request.error);
      });
      db.close();
    } catch (_) { root = null; }
  }
  async function recording(source) {
    const url = new URL(source, location.href);
    if (url.protocol !== 'file:') throw new Error(l.missing);
    const parts = url.pathname.split('/').filter(Boolean).map(decodeURIComponent);
    const index = parts.lastIndexOf(root.name);
    if (index < 0 || index === parts.length - 1) throw new Error(l.missing);
    let directory = root;
    for (const part of parts.slice(index + 1, -1)) {
      if (part === '.' || part === '..') throw new Error(l.missing);
      directory = await directory.getDirectoryHandle(part);
    }
    const name = parts.at(-1);
    const handle = await directory.getFileHandle(name);
    return {file:await handle.getFile(), directory, name};
  }
  function clock(seconds) {
    const s = Math.max(0, Math.floor(seconds));
    return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
  }
  function validSegments(segments) {
    return Array.isArray(segments) && segments.length <= 50000 && segments.every(s =>
      typeof s.text === 'string' && s.text.length < 20000 && Number.isFinite(s.start) &&
      Number.isFinite(s.end) && s.start >= 0 && s.end >= s.start);
  }
  function showLine(card, segment) {
    const row = document.createElement('p'); row.className = 'transcribe-line';
    row.dataset.start = String(segment.start); row.dataset.end = String(segment.end);
    const time = document.createElement('button'); time.type = 'button'; time.textContent = clock(segment.start);
    time.addEventListener('click', () => {
      card.closest('details').querySelectorAll('video').forEach(video => {video.currentTime = segment.start;});
    });
    const text = document.createElement('span'); text.textContent = segment.text;
    row.append(time, text); card.querySelector('.transcribe-lines').append(row);
  }
  function saveText(card) {
    const text = card.segments.map(s => '[' + clock(s.start) + '] ' + s.text).join('\n');
    const url = URL.createObjectURL(new Blob([text], {type:'text/plain;charset=utf-8'}));
    const a = document.createElement('a'); a.href = url;
    a.download = 'transcript.txt'; a.click(); setTimeout(() => URL.revokeObjectURL(url), 10000);
  }
  async function loadSaved(card) {
    if (!root || await root.queryPermission({mode:'readwrite'}) !== 'granted') return;
    try {
      const entry = await recording(card.dataset.transcribeSrc);
      const file = await (await entry.directory.getFileHandle(entry.name + '.okuflow-transcript.json')).getFile();
      if (file.size > 8 * 1024 * 1024) return;
      const data = JSON.parse(await file.text());
      if (data.version !== 1 || data.recording?.size !== entry.file.size ||
          data.recording?.lastModified !== entry.file.lastModified || !validSegments(data.segments)) return;
      card.segments = data.segments;
      card.querySelector('.transcribe-lines').replaceChildren();
      card.segments.forEach(s => showLine(card, s));
      card.querySelector('.transcribe-result').hidden = false;
      card.querySelector('.transcribe-export').hidden = !card.segments.length;
      card.querySelector('.transcribe-status').textContent = l.saved;
    } catch (_) { /* No saved transcript for this recording. */ }
  }
  async function begin(card) {
    const status = card.querySelector('.transcribe-status');
    const button = card.querySelector('.transcribe-start');
    if (!window.showDirectoryPicker) { status.textContent = l.unsupported; return; }
    try {
      if (!root || await root.queryPermission({mode:'readwrite'}) !== 'granted') {
        if (root && await root.requestPermission({mode:'readwrite'}) === 'granted') {
          status.textContent = l.ready;
        } else {
          status.textContent = l.choose;
          root = await showDirectoryPicker({id:'okuflow-notes', mode:'readwrite'});
          await remember(root); status.textContent = l.ready;
        }
        await loadSaved(card);
        return; // Opening a tab consumes activation: keep permission as its own click.
      }
      if (!popup || popup.closed) popup = window.open(toolUrl, 'okuflow-transcribe-' + crypto.randomUUID());
      else popup.focus();
      if (!popup) { status.textContent = l.blocked; return; }
      status.textContent = l.waiting; button.disabled = true;
      const entry = await recording(card.dataset.transcribeSrc);
      card.segments = []; card.querySelector('.transcribe-lines').replaceChildren();
      card.querySelector('.transcribe-result').hidden = false;
      card.querySelector('.transcribe-export').hidden = true;
      const stop = card.querySelector('.transcribe-stop'); stop.hidden = false;
      const channel = new MessageChannel();
      let complete = false;
      let receiveReady;
      const finish = () => {complete = true; window.removeEventListener('message', receiveReady); clearInterval(ping); clearTimeout(timeout); button.disabled = false; stop.hidden = true; channel.port1.close();};
      stop.onclick = () => {channel.port1.postMessage({type:'cancel'}); status.textContent = l.stopped; finish();};
      channel.port1.onmessage = async ({data}) => {
        if (complete || !data || typeof data !== 'object') return;
        if (data.type === 'segment' && validSegments([data])) {card.segments.push({start:data.start,end:data.end,text:data.text}); showLine(card, data);}
        if (data.type === 'status') {
          const phase = {queued:l.queued, loading:l.loading, downloading:l.downloading, decoding:l.reading, transcribing:l.running, cancelled:l.stopped};
          const percent = Number.isFinite(data.fraction) ? ' ' + Math.round(Math.min(1, Math.max(0, data.fraction)) * 100) + '%' : '';
          status.textContent = (phase[data.state] || l.waiting) + percent;
        }
        if (data.type === 'error' || data.type === 'cancelled') {status.textContent = data.type === 'cancelled' ? l.stopped : l.failed + ' ' + String(data.message || ''); finish();}
        if (data.type === 'done' && validSegments(data.segments)) {
          card.segments = data.segments; card.querySelector('.transcribe-lines').replaceChildren();
          card.segments.forEach(s => showLine(card, s));
          card.querySelector('.transcribe-export').hidden = !card.segments.length;
          status.textContent = card.segments.length ? l.done : l.empty;
          finish();
          try {
            const handle = await entry.directory.getFileHandle(entry.name + '.okuflow-transcript.json', {create:true});
            const writable = await handle.createWritable();
            await writable.write(JSON.stringify({version:1, recording:{name:entry.name,size:entry.file.size,lastModified:entry.file.lastModified}, model:data.model,device:data.device,language:data.language,segments:card.segments}));
            await writable.close(); status.textContent = l.saved;
          } catch (_) {status.textContent = l.saveFailed;}
        }
      };
      receiveReady = event => {
        if (event.source !== popup || event.origin !== toolOrigin || event.data?.type !== 'okuflow-transcribe-ready' || event.data.version !== 1) return;
        window.removeEventListener('message', receiveReady); clearTimeout(timeout);
        popup.postMessage({type:'okuflow-transcribe-request', version:1, file:entry.file, title:document.title, language:lang}, toolOrigin, [channel.port2]);
      };
      window.addEventListener('message', receiveReady);
      const ping = setInterval(() => {
        if (popup.closed) {status.textContent = l.connection; window.removeEventListener('message', receiveReady); finish();}
        else popup.postMessage({type:'okuflow-transcribe-ping'}, toolOrigin);
      }, 500);
      const timeout = setTimeout(() => {status.textContent = l.connection; window.removeEventListener('message', receiveReady); finish();}, 30000);
      popup.postMessage({type:'okuflow-transcribe-ping'}, toolOrigin);
    } catch (error) {
      button.disabled = false;
      if (error?.name !== 'AbortError') {root = null; status.textContent = l.missing;}
    }
  }
  document.addEventListener('DOMContentLoaded', async () => {
    const cards = [...document.querySelectorAll('[data-transcribe-src]')];
    for (const card of cards) {
      card.segments = [];
      card.querySelector('.transcribe-start').textContent = l.go;
      card.querySelector('.transcribe-stop').textContent = l.stop;
      card.querySelector('.transcribe-export').textContent = l.text;
      card.querySelector('.transcribe-heading').textContent = l.title;
      card.querySelector('.transcribe-start').addEventListener('click', () => begin(card));
      card.querySelector('.transcribe-export').addEventListener('click', () => saveText(card));
    }
    await recall();
    await Promise.all(cards.map(loadSaved));
  });
})();
</script>
)JS";

} // namespace okuflow
