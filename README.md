Клиент-серверное приложение на языке C под linux.

Клиент:
1. Приложение запускается с параметрами:
ip = IP-адрес сервера;
path = путь к директории.
Если приложение запущено без параметра path, path = каталог, из которого запущено приложение;
2. Клиент читает все записи из каталога path и передает на сервер информацию обо всех фалах в этом каталоге (рекурсивно, включая подкаталоги).
Информация о файлах включает следующие параметры:
fname = имя файла (включает полный путь внутри каталога path);
fsize = размер файла;
ftime = дата создания файла.
fblock = массив байтов длиной len (только для файлов длиной > 0 ):
для файлов размером <= 32 байта, len = fsize/2+1, считанный со смещением с конца файла offset = fsize/2+1.
для файлов размером > 32 байт, len = 32 байта, считанных со смещением offset = 32 байта с конца файла.
3. После передачи всей информации приложение клиент завершает свою работу.

Сервер:
1. Сервер принимает подключения со всех сетевых интерфейсов;
2. Сервер принимает информацию от клиентов и складывает её в текстовый файл: path = <IP-адрес клиента>_files.txt.
3. Сервер стартует с параметром config_file, в котором должно быть указано имя файла содержащего конфигурацию сервера: номер порта, который слушает сервер, ip адрес, с которого принимаются подключения и максимальное одновременное количество клиентов. Если файл не указан применяются умолчательные значения: порт 59000, ip 0.0.0.0, 1024 клиента (максимальное количество в принципе на сервере, если в конфиге задать больше - будет обрезано до 1024).

Часть кода сервера взята здесь: https://habrahabr.ru/post/129207/

