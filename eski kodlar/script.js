//================================================================================================
// akilli otopark simülasyonu - ana script dosyasi
//================================================================================================

//------------------------------------------------------------------------------------------------
// modül importlari
//------------------------------------------------------------------------------------------------

// firebase veritabanı bağlantısını ve yapılandırmasını içe aktarır.
import { db } from './firebase-config.js';
// firestore veritabanı işlemleri için gerekli olan fonksiyonları (veri okuma, yazma, dinleme vb.) firebase sdk'sinden içe aktarır.
import { collection, getDocs, setDoc, doc, onSnapshot } from "https://www.gstatic.com/firebasejs/9.6.1/firebase-firestore.js";

//------------------------------------------------------------------------------------------------
// global değişkenler
//------------------------------------------------------------------------------------------------

// otoparktaki toplam kat sayısını belirten sabit bir değişken.
const TOTAL_FLOORS = 3;
// her bir katın park yeri verilerini (id, durum, koordinat vb.) saklamak için kullanılan bir nesne.
const parkingDataByFloor = {};
// otoparktaki tüm park yerlerinin bir listesini tutan bir dizi.
let allParkingSpots = [];
// simülasyonun periyodik olarak çalışmasını sağlayan interval'i tutar.
let simulationInterval = null;
// en yakın boş park yerini bulma işlemini periyodik olarak çalıştıran interval'i tutar.
let nearestSpotInterval = null;

// kullanıcı arayüzündeki (ui) elemanlara kolay erişim sağlamak için kullanılan bir nesne.
const ui = {
    startSimBtn: null, // simülasyonu başlatma butonu
    stopSimBtn: null,  // simülasyonu durdurma butonu
    totalSpotsSpan: null, // toplam park yeri sayısını gösteren alan
    occupiedSpotsSpan: null, // dolu park yeri sayısını gösteren alan
    availableSpotsSpan: null, // boş park yeri sayısını gösteren alan
    occupancyRateSpan: null, // doluluk oranını gösteren alan
    nearestSpotDisplay: null // en yakın boş park yerini gösteren alan
};

//------------------------------------------------------------------------------------------------
// firebase işlemleri
//------------------------------------------------------------------------------------------------

// firebase ile ilgili tüm veritabanı operasyonlarını içeren bir nesne.
const firebaseOps = {
    // başlangıçta firestore veritabanından mevcut park yeri verilerini yükler.
    async loadParkingData() {
        try {
            // 'parkingSpots' koleksiyonundaki tüm dökümanları alır.
            const parkingSnapshot = await getDocs(collection(db, 'parkingSpots'));
            const existingSpots = new Map();
            // her bir dökümanı map'e ekler (id -> veri). bu, verilere hızlı erişim sağlar.
            parkingSnapshot.forEach(doc => existingSpots.set(doc.id, doc.data()));
            return existingSpots;
        } catch (error) {
            // veri yükleme sırasında bir hata olursa konsola yazar.
            console.error("firebase veri yükleme hatası:", error);
            return new Map(); // hata durumunda boş bir map döndürür.
        }
    },
    // belirli bir park yerinin doluluk durumunu (dolu/boş) firestore'da günceller.
    async updateSpotStatus(spotId, isOccupied) {
        try {
            // ilgili park yerinin dökümanına yeni durumu ve son güncellenme zamanını yazar.
            await setDoc(doc(db, 'parkingSpots', spotId), { occupied: isOccupied, lastUpdated: new Date().toISOString() });
        } catch (error) {
            // güncelleme sırasında bir hata olursa konsola yazar.
            console.error(`park yeri ${spotId} güncellenirken hata:`, error);
        }
    },
    // firestore'daki 'parkingSpots' koleksiyonunu dinleyerek gerçek zamanlı güncellemeleri alır.
    setupRealtimeUpdates() {
        onSnapshot(collection(db, 'parkingSpots'), (snapshot) => {
            let needsUpdate = false; // arayüzün güncellenip güncellenmeyeceğini kontrol eder.
            // koleksiyondaki değişiklikleri (ekleme, silme, güncelleme) döngüye alır.
            snapshot.docChanges().forEach((change) => {
                // yerel `allParkingSpots` dizisinde ilgili park yerini bulur.
                const spot = allParkingSpots.find(s => s.id === change.doc.id);
                // eğer park yeri bulunduysa ve doluluk durumu değiştiyse, yerel veriyi günceller.
                if (spot && spot.occupied !== change.doc.data().occupied) {
                    spot.occupied = change.doc.data().occupied;
                    needsUpdate = true; // arayüzün güncellenmesi gerektiğini işaretler.
                }
            });
            // eğer en az bir değişiklik olduysa, otopark görünümünü günceller.
            if (needsUpdate) updateParkingLotDisplay();
        });
    }
};

//------------------------------------------------------------------------------------------------
// otopark işlemleri
//------------------------------------------------------------------------------------------------

// otoparkın genel işleyişini yöneten fonksiyonları içeren nesne.
const parkingOps = {
    // otoparkı başlatan ana fonksiyon. svg'lerden verileri okur, firebase ile senkronize eder.
    async initialize() {
        // firebase'den mevcut park yeri durumlarını yükler.
        const existingSpots = await firebaseOps.loadParkingData();
        // her kat için döngü başlatır.
        for (let floor = 1; floor <= TOTAL_FLOORS; floor++) {
            const svgObject = document.getElementById(`svg-object-${floor}`);
            const svgDoc = svgObject.contentDocument; // <object> içindeki svg dökümanına erişir.
            if (!svgDoc) continue; // eğer svg yüklenmediyse bu katı atlar.

            parkingDataByFloor[floor] = []; // bu kat için boş bir dizi oluşturur.
            // svg içindeki tüm park yeri gruplarını (`<g>`) seçer.
            const svgParkSpots = svgDoc.querySelectorAll('g[class*="parking-spot-svg"]');
            
            // her bir park yeri elementi için döngü.
            svgParkSpots.forEach(spotElement => {
                const rect = spotElement.querySelector('rect'); // park yerini temsil eden dörtgeni bulur.
                const spotId = spotElement.id; // park yerinin benzersiz id'sini alır.
                if (!rect || !spotId) return; // gerekli elemanlar yoksa bu park yerini atlar.

                // firebase'den gelen veri varsa onu, yoksa varsayılan olarak `occupied: false` kullanır.
                const spotData = existingSpots.get(spotId) || { occupied: false };
                // park yeri verilerini `parkingDataByFloor` nesnesine ekler.
                parkingDataByFloor[floor].push({
                    id: spotId,
                    floor: floor,
                    occupied: spotData.occupied,
                    x: parseFloat(rect.getAttribute('x')), // koordinatları alır.
                    y: parseFloat(rect.getAttribute('y'))
                });
            });
        }
        // tüm katlardaki park yerlerini tek bir `allParkingSpots` dizisinde birleştirir.
        allParkingSpots = Object.values(parkingDataByFloor).flat();
        // firebase'den gelecek anlık güncellemeleri dinlemeye başlar.
        firebaseOps.setupRealtimeUpdates();
        // otoparkın arayüzdeki görünümünü ilk kez günceller.
        updateParkingLotDisplay();
        // en yakın boş yeri bulma işlemini başlatır.
        startNearestSpotFinder();
    },

    // otopark girişine en yakın boş park yerini bulur.
    findNearestSpot() {
        // önceki aramadan kalan vurguları temizler.
        for (let floor = 1; floor <= TOTAL_FLOORS; floor++) {
            const svgDoc = document.getElementById(`svg-object-${floor}`)?.contentDocument;
            if (svgDoc) {
                svgDoc.querySelectorAll('.highlight').forEach(el => el.classList.remove('highlight'));
            }
        }

        let nearestSpot = null; // en yakın boş park yerini tutacak değişken.

        // en boş ve en alt kattan başlayarak en yakın yeri arar.
        for (let floor = 1; floor <= TOTAL_FLOORS; floor++) {
            const floorData = parkingDataByFloor[floor];
            if (!floorData || floorData.length === 0) continue; // katta park yeri yoksa atlar.

            // her katın giriş noktası olarak a1 park yerini varsayar.
            const entryPoint = floorData.find(spot => spot.id.endsWith('-A1'));
            if (!entryPoint) continue; // giriş noktası bulunamazsa bu katı atlar.

            // kattaki boş park yerlerini bulur.
            const availableSpots = floorData.filter(spot => !spot.occupied);
            if (availableSpots.length === 0) continue; // boş yer yoksa sonraki kata geçer.

            let floorNearestSpot = null, minDistance = Infinity;
            // boş park yerleri arasında giriş noktasına en yakın olanı bulur (öklid mesafesi).
            for (const spot of availableSpots) {
                const distance = Math.hypot(spot.x - entryPoint.x, spot.y - entryPoint.y);
                if (distance < minDistance) {
                    minDistance = distance;
                    floorNearestSpot = spot;
                }
            }
            // eğer bu katta en yakın yer bulunduysa, aramayı durdurur ve bu yeri kullanır.
            if (floorNearestSpot) {
                nearestSpot = floorNearestSpot;
                break;
            }
        }

        // en yakın boş yer bulunduysa arayüzde gösterir ve svg üzerinde vurgular.
        if (nearestSpot) {
            const svgDoc = document.getElementById(`svg-object-${nearestSpot.floor}`)?.contentDocument;
            const spotElement = svgDoc?.getElementById(nearestSpot.id);
            if (spotElement) {
                const spotNumber = spotElement.querySelector('text')?.textContent || nearestSpot.id.split('-')[1];
                ui.nearestSpotDisplay.textContent = `kat ${nearestSpot.floor} - ${spotNumber}`;
                spotElement.classList.add('highlight'); // css ile vurgulama yapar.
            }
        } else {
            // hiç boş yer yoksa bilgi mesajı gösterir.
            ui.nearestSpotDisplay.textContent = 'tüm katlar dolu';
        }
    }
};

//------------------------------------------------------------------------------------------------
// simülasyon işlemleri
//------------------------------------------------------------------------------------------------

// otopark doluluk oranını rastgele değiştiren simülasyon fonksiyonlarını içerir.
const simulationOps = {
    // belirtilen sayıda arabayı otoparka ekler veya çıkarır.
    async changeCarCount(action, quantity) {
        // 'add' ise boş yerleri, 'remove' ise dolu yerleri hedefler.
        const spots = (action === 'add') 
            ? allParkingSpots.filter(s => !s.occupied)
            : allParkingSpots.filter(s => s.occupied);
        
        // hedeflenen yerlerden rastgele `quantity` kadarını seçer.
        const spotsToChange = spots.sort(() => 0.5 - Math.random()).slice(0, quantity);

        // seçilen yerlerin durumunu günceller ve firebase'e yazar.
        for (const spot of spotsToChange) {
            spot.occupied = (action === 'add');
            firebaseOps.updateSpotStatus(spot.id, spot.occupied);
        }
        // arayüzü günceller.
        updateParkingLotDisplay();
    },

    // simülasyonun her bir adımını çalıştırır.
    async runSimulationStep() {
        // değiştirilecek araç sayısını rastgele belirler (toplamın %10'u kadar veya en az 5).
        const maxChange = Math.max(5, Math.floor(allParkingSpots.length * 0.1));
        const quantity = Math.floor(Math.random() * maxChange) + 1;
        // mevcut doluluk oranını hesaplar.
        const occupancyRate = allParkingSpots.length > 0 ? allParkingSpots.filter(spot => spot.occupied).length / allParkingSpots.length : 0;

        // doluluk oranına göre araç ekleme veya çıkarma olasılığını ayarlar.
        let addProbability = 0.5; // varsayılan olasılık.
        if (occupancyRate > 0.8) addProbability = 0.2; // otopark çok doluysa araç ekleme olasılığını düşürür.
        else if (occupancyRate < 0.2) addProbability = 0.8; // otopark çok boşsa araç ekleme olasılığını artırır.

        // belirlenen olasılığa göre araç ekler veya çıkarır.
        await this.changeCarCount(Math.random() < addProbability ? 'add' : 'remove', quantity);
    }
};

//------------------------------------------------------------------------------------------------
// arayüz güncelleme fonksiyonlari
//------------------------------------------------------------------------------------------------

// otoparktaki verileri (doluluk, sayılar vb.) arayüzde gösterir.
function updateParkingLotDisplay() {
    let totalOccupiedCount = 0;

    // her kat için döngü.
    for (let floor = 1; floor <= TOTAL_FLOORS; floor++) {
        const floorData = parkingDataByFloor[floor];
        const svgDoc = document.getElementById(`svg-object-${floor}`)?.contentDocument;
        if (!floorData || !svgDoc) continue; // veri veya svg yoksa atlar.

        // kattaki her park yeri için.
        for (const spot of floorData) {
            const spotElement = svgDoc.getElementById(spot.id);
            if (spotElement) {
                // doluluk durumuna göre css sınıflarını (`occupied` veya `available`) ekler/kaldırır.
                spotElement.classList.toggle('occupied', spot.occupied);
                spotElement.classList.toggle('available', !spot.occupied);
            }
        }
        // bu kattaki dolu park yeri sayısını toplama ekler.
        totalOccupiedCount += floorData.filter(s => s.occupied).length;
    }

    // genel istatistikleri (toplam, dolu, boş, doluluk oranı) günceller.
    const totalSpots = allParkingSpots.length;
    ui.totalSpotsSpan.textContent = totalSpots;
    ui.occupiedSpotsSpan.textContent = totalOccupiedCount;
    ui.availableSpotsSpan.textContent = totalSpots - totalOccupiedCount;
    ui.occupancyRateSpan.textContent = `${totalSpots > 0 ? ((totalOccupiedCount / totalSpots) * 100).toFixed(1) : 0}%`;
}

//------------------------------------------------------------------------------------------------
// kontrol fonksiyonlari
//------------------------------------------------------------------------------------------------

// en yakın boş yeri bulma işlemini periyodik olarak başlatır.
function startNearestSpotFinder() {
    if (nearestSpotInterval) clearInterval(nearestSpotInterval); // eğer zaten çalışıyorsa eskisini durdurur.
    parkingOps.findNearestSpot(); // ilk aramayı hemen yapar.
    nearestSpotInterval = setInterval(parkingOps.findNearestSpot, 2000); // her 2 saniyede bir tekrarlar.
}

// simülasyonu başlatır.
function startSimulation() {
    if (simulationInterval) return; // zaten çalışıyorsa bir şey yapmaz.
    ui.startSimBtn.disabled = true; // başlatma butonunu pasif hale getirir.
    ui.stopSimBtn.disabled = false; // durdurma butonunu aktif hale getirir.
    simulationOps.runSimulationStep(); // ilk adımı hemen çalıştırır.
    simulationInterval = setInterval(() => simulationOps.runSimulationStep(), 4000); // her 4 saniyede bir tekrarlar.
}

// simülasyonu durdurur.
function stopSimulation() {
    if (!simulationInterval) return; // çalışmıyorsa bir şey yapmaz.
    clearInterval(simulationInterval); // periyodik çalışmayı durdurur.
    simulationInterval = null;
    ui.startSimBtn.disabled = false; // başlatma butonunu tekrar aktif hale getirir.
    ui.stopSimBtn.disabled = true; // durdurma butonunu pasif hale getirir.
}

//------------------------------------------------------------------------------------------------
// güvenilir svg yükleyici
//------------------------------------------------------------------------------------------------

// tüm kat planı svg'lerinin yüklenmesini bekleyen bir fonksiyon.
function waitForSvgLoads() {
    const svgLoadPromises = []; // her bir svg yüklemesi için bir promise tutacak dizi.
    for (let i = 1; i <= TOTAL_FLOORS; i++) {
        const svgObject = document.getElementById(`svg-object-${i}`);
        if (svgObject) {
            // eğer svg zaten yüklenmişse (örneğin cache'den geldiyse), beklemeden devam et.
            if (svgObject.contentDocument && svgObject.contentDocument.readyState === 'complete') {
                console.log(`kat planı ${i} zaten yüklü.`);
                svgLoadPromises.push(Promise.resolve());
                continue;
            }
            
            // her bir svg için yeni bir promise oluşturur.
            const promise = new Promise((resolve, reject) => {
                // svg başarıyla yüklendiğinde promise'i resolve eder.
                svgObject.addEventListener('load', () => {
                    console.log(`kat planı ${i} başarıyla yüklendi.`);
                    resolve();
                });
                // svg yüklenirken hata olursa promise'i reject eder.
                svgObject.addEventListener('error', () => {
                    console.error(`kat planı ${i} yüklenirken hata oluştu.`);
                    reject(new Error(`svg ${i} could not be loaded.`));
                });
            });
            svgLoadPromises.push(promise);
        } else {
            // eğer bir katın <object> elementi html'de bulunamazsa, bu ciddi bir hatadır.
            console.error(`kat planı ${i} için <object> elementi bulunamadı.`);
            svgLoadPromises.push(Promise.reject(new Error(`object for floor ${i} not found.`)));
        }
    }
    // tüm promise'lerin tamamlanmasını bekler. biri bile hata verirse `promise.all` reject olur.
    return Promise.all(svgLoadPromises);
}


//------------------------------------------------------------------------------------------------
// sayfa yüklendiğinde çalişacak ana kod
//------------------------------------------------------------------------------------------------

// tarayıcı penceresinin içeriği tamamen yüklendiğinde bu fonksiyon çalışır.
window.addEventListener('load', async () => {
    // arayüz elemanlarını id'lerine göre seçip `ui` nesnesine atar.
    ui.startSimBtn = document.getElementById('start-simulation-btn');
    ui.stopSimBtn = document.getElementById('stop-simulation-btn');
    ui.totalSpotsSpan = document.getElementById('total-park-spots');
    ui.occupiedSpotsSpan = document.getElementById('occupied-park-spots');
    ui.availableSpotsSpan = document.getElementById('available-park-spots');
    ui.occupancyRateSpan = document.getElementById('occupancy-rate');
    ui.nearestSpotDisplay = document.getElementById('nearest-spot-display');

    try {
        console.log("sayfa ana yapısı yüklendi. kat planlarının yüklenmesi bekleniyor...");
        // otoparkı başlatmadan önce tüm svg'lerin yüklenmesini bekler.
        await waitForSvgLoads();
        console.log("tüm kat planları başarıyla yüklendi. otopark başlatılıyor...");
        // tüm svg'ler yüklendikten sonra otoparkı başlatır.
        await parkingOps.initialize();
    } catch (error) {
        // svg yükleme veya otopark başlatma sırasında bir hata olursa konsola yazar.
        console.error("otopark başlatılırken kritik bir hata oluştu:", error);
        // kullanıcıya bir hata mesajı göstermek iyi bir fikir olabilir.
        // örneğin: document.body.innerHTML = '<h1>otopark planı yüklenemedi. lütfen sayfayı yenileyin.</h1>';
    }

    // kontrol butonlarına tıklama olay dinleyicilerini atar.
    ui.startSimBtn.addEventListener('click', startSimulation);
    ui.stopSimBtn.addEventListener('click', stopSimulation);
    ui.stopSimBtn.disabled = true; // başlangıçta durdurma butonu pasif.

    // kat sekmeleri (tabs) arasında geçiş yapıldığında, görünümün güncellenmesini sağlar.
    document.querySelectorAll('#floor-tabs .nav-link').forEach(tab => {
        tab.addEventListener('shown.bs.tab', updateParkingLotDisplay);
    });
});
