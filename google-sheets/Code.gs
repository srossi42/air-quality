/**
 * Station air — historique et tableau de bord dans Google Sheets.
 *
 * - L'écran intérieur envoie toutes les 5 min un POST JSON à ce script, publié
 *   en « Application Web » : chaque envoi ajoute une ligne à l'onglet « Mesures ».
 * - L'onglet « Tableau de bord » affiche les courbes intérieur + extérieur :
 *   période, pas d'agrégation et échelles se règlent dans ses cellules, et les
 *   graphes se recalculent à chaque modification.
 *
 * L'URL de l'application est publique : le jeton empêche quiconque d'autre
 * d'écrire dans le tableau. Même valeur que SHEETS_TOKEN dans secrets.h.
 * Il est rangé dans les propriétés du script (Paramètres du projet →
 * Propriétés du script → TOKEN), jamais dans ce fichier publié sur Git.
 */
const TOKEN = PropertiesService.getScriptProperties().getProperty('TOKEN');

const MEASURES = 'Mesures';
const DASHBOARD = 'Tableau de bord';
const VIEW = 'Vue'; // données calculées pour les graphes, onglet masqué

// Colonnes de l'onglet Mesures. `key` = champ du JSON envoyé par l'écran.
// `agg` = regroupement quand plusieurs mesures tombent dans le même pas.
const COLUMNS = [
  { key: null, title: 'Horodatage' },
  { key: 'co2', title: 'CO2 (ppm)', agg: 'avg' },
  { key: 'cov', title: 'COV (indice)', agg: 'avg' },
  { key: 't_int', title: 'Temp. int. (°C)', agg: 'avg' },
  { key: 'h_int', title: 'Hum. int. (%)', agg: 'avg' },
  { key: 't_ext', title: 'Temp. ext. (°C)', agg: 'avg' },
  { key: 'h_ext', title: 'Hum. ext. (%)', agg: 'avg' },
  { key: 'pression', title: 'Pression (hPa)', agg: 'avg' },
  { key: 'pm1', title: 'PM1 (µg/m³)', agg: 'avg' },
  { key: 'pm25', title: 'PM2.5 (µg/m³)', agg: 'avg' },
  { key: 'pm10', title: 'PM10 (µg/m³)', agg: 'avg' },
  { key: 'pm10_max', title: 'PM10 max (µg/m³)', agg: 'max' },
  { key: 'tendance', title: 'Tendance 3 h (hPa)', agg: 'avg' },
  { key: 'verdict', title: 'Verdict' },
  { key: 'chantier', title: 'Mode chantier' },
  { key: 'sonde', title: 'Sonde reçue' },
  { key: 'rx_espnow', title: 'Reçus ESP-NOW', agg: 'sum' },
  { key: 'rx_wifi', title: 'Reçus Wi-Fi', agg: 'sum' },
  // Diagnostic : redémarrages et signal Wi-Fi des deux boîtiers.
  { key: 'ecran_boot', title: 'Écran : démarrages' },
  { key: 'ecran_reset', title: 'Écran : cause du démarrage' },
  { key: 'ecran_uptime_min', title: 'Écran : allumé depuis (min)' },
  { key: 'ecran_rssi', title: 'Écran : Wi-Fi (dBm)' },
  { key: 'sonde_reset', title: 'Sonde : cause du démarrage' },
  { key: 'sonde_uptime_min', title: 'Sonde : allumée depuis (min)' },
  { key: 'sonde_rssi', title: 'Sonde : Wi-Fi (dBm)' },
];

// Alerte par mail : plus aucune ligne, ou sonde muette, depuis ce délai.
const ALERT_AFTER_MINUTES = 30;

// Seuils tracés en repère sur les graphes.
const THRESHOLDS = [
  { title: 'Seuil aération (1000 ppm)', value: 1000 },
  { title: 'Limite UE PM10 jour (50)', value: 50 },
];

// Un graphe = une liste de séries, désignées par le titre de leur colonne.
const CHARTS = [
  { title: 'CO2 intérieur (ppm)', series: ['CO2 (ppm)', 'Seuil aération (1000 ppm)'] },
  {
    title: 'Particules extérieures (µg/m³)',
    series: ['PM1 (µg/m³)', 'PM2.5 (µg/m³)', 'PM10 (µg/m³)', 'PM10 max (µg/m³)', 'Limite UE PM10 jour (50)'],
  },
  { title: 'Températures (°C)', series: ['Temp. int. (°C)', 'Temp. ext. (°C)'] },
  { title: 'Humidité (%)', series: ['Hum. int. (%)', 'Hum. ext. (%)'] },
  { title: 'Pression extérieure (hPa)', series: ['Pression (hPa)'] },
  { title: 'Indice COV intérieur', series: ['COV (indice)'] },
  { title: 'Réception de la sonde (paquets)', series: ['Reçus ESP-NOW', 'Reçus Wi-Fi'] },
];

const PERIODS = { '24 heures': 1, '3 jours': 3, '7 jours': 7, '30 jours': 30, 'Tout': null, 'Personnalisée': null };
const STEPS = { 'Auto': 0, 'Brut (5 min)': 5, '15 min': 15, '1 heure': 60, '1 jour': 1440 };
const MAX_POINTS = 1500; // au-delà, les graphes deviennent lents

// ==========================================
// RÉCEPTION DES MESURES
// ==========================================
function doPost(e) {
  let data;
  try {
    data = JSON.parse(e.postData.contents);
  } catch (error) {
    return reply({ ok: false, error: 'JSON invalide' });
  }
  if (!TOKEN || data.token !== TOKEN) return reply({ ok: false, error: 'jeton refusé' });

  // L'heure du serveur Google fait foi : pas besoin d'horloge côté écran.
  const row = COLUMNS.map((column) => {
    if (column.key === null) return new Date();
    const value = data[column.key];
    return value === null || value === undefined ? '' : value;
  });
  measuresSheet().appendRow(row);
  return reply({ ok: true });
}

/** Onglet des mesures ; en-têtes créés, ou complétés si de nouvelles colonnes sont apparues. */
function measuresSheet() {
  const spreadsheet = SpreadsheetApp.getActiveSpreadsheet();
  let sheet = spreadsheet.getSheetByName(MEASURES);
  if (!sheet) {
    sheet = spreadsheet.insertSheet(MEASURES);
    sheet.setFrozenRows(1);
    sheet.getRange('A:A').setNumberFormat('dd/MM/yyyy HH:mm');
  }
  if (sheet.getLastColumn() < COLUMNS.length) {
    sheet.getRange(1, 1, 1, COLUMNS.length).setValues([COLUMNS.map((c) => c.title)]);
  }
  return sheet;
}

function reply(object) {
  return ContentService.createTextOutput(JSON.stringify(object)).setMimeType(
    ContentService.MimeType.JSON,
  );
}

// ==========================================
// MENU ET DÉCLENCHEURS
// ==========================================
function onOpen() {
  SpreadsheetApp.getUi()
    .createMenu('Station air')
    .addItem('Actualiser le tableau de bord', 'refreshDashboard')
    .addItem('Installer le tableau de bord', 'installer')
    .addToUi();
}

/** Toute modification des réglages du tableau de bord recalcule les graphes. */
function onEdit(e) {
  const sheet = e.range.getSheet();
  if (sheet.getName() !== DASHBOARD) return;
  if (e.range.getColumn() > 3) return; // seules les colonnes de réglages A à C
  refreshDashboard();
}

/**
 * À lancer une fois depuis le menu Station air (ou l'éditeur) : crée le tableau
 * de bord, son actualisation toutes les 30 minutes, et la surveillance de la
 * réception toutes les 15 minutes. Sans effet sur ce qui existe déjà.
 */
function installer() {
  setupDashboard();
  ensureTrigger('refreshDashboard', 30);
  ensureTrigger('verifierReception', 15);
  refreshDashboard();
}

function ensureTrigger(handler, minutes) {
  const exists = ScriptApp.getProjectTriggers().some(
    (trigger) => trigger.getHandlerFunction() === handler,
  );
  if (!exists) ScriptApp.newTrigger(handler).timeBased().everyMinutes(minutes).create();
}

// ==========================================
// ALERTE PAR MAIL
// ==========================================
/**
 * Prévient par mail si l'écran n'envoie plus de lignes, ou si toutes les lignes
 * de la dernière demi-heure disent « sonde non reçue ». Un seul mail par panne,
 * puis un mail au retour à la normale.
 */
function verifierReception() {
  const sheet = measuresSheet();
  const last = sheet.getLastRow();
  if (last < 2) return;

  const lastTime = new Date(sheet.getRange(last, 1).getValue());
  const silentMinutes = Math.round((Date.now() - lastTime.getTime()) / 60000);

  const sondeColumn = COLUMNS.findIndex((c) => c.key === 'sonde') + 1;
  const recent = Math.min(last - 1, Math.round(ALERT_AFTER_MINUTES / 5));
  const sondeValues = sheet.getRange(last - recent + 1, sondeColumn, recent, 1).getValues();
  const sondeMuette = sondeValues.every(([value]) => value === false || value === 'FALSE');

  let probleme = '';
  if (silentMinutes > ALERT_AFTER_MINUTES) {
    probleme = `L'écran n'a envoyé aucune ligne depuis ${silentMinutes} min (dernière : ${formatDate(lastTime)}).`;
  } else if (sondeMuette) {
    probleme = `L'écran ne reçoit plus la sonde depuis au moins ${ALERT_AFTER_MINUTES} min.`;
  }

  const properties = PropertiesService.getScriptProperties();
  const enPanne = properties.getProperty('alerte') === 'oui';
  const destinataire = Session.getEffectiveUser().getEmail();
  if (probleme && !enPanne) {
    MailApp.sendEmail(destinataire, 'Station air : panne', probleme);
    properties.setProperty('alerte', 'oui');
  } else if (!probleme && enPanne) {
    MailApp.sendEmail(
      destinataire,
      'Station air : retour à la normale',
      `Dernière ligne reçue : ${formatDate(lastTime)}.`,
    );
    properties.deleteProperty('alerte');
  }
}

function formatDate(date) {
  return Utilities.formatDate(date, Session.getScriptTimeZone(), 'dd/MM/yyyy HH:mm');
}

// ==========================================
// TABLEAU DE BORD
// ==========================================
const CELL_PERIOD = 'B3';
const CELL_FROM = 'B4';
const CELL_TO = 'B5';
const CELL_STEP = 'B6';
const SCALES_FIRST_ROW = 10; // une ligne par graphe : titre | Y min | Y max

function setupDashboard() {
  const spreadsheet = SpreadsheetApp.getActiveSpreadsheet();
  const sheet = spreadsheet.getSheetByName(DASHBOARD) || spreadsheet.insertSheet(DASHBOARD, 0);

  sheet.getRange('A1').setValue('Station air').setFontSize(16).setFontWeight('bold');
  sheet.getRange('A3:A6').setValues([['Période'], ['Du'], ['Au'], ['Pas']]).setFontWeight('bold');

  const period = sheet.getRange(CELL_PERIOD);
  period.setDataValidation(
    SpreadsheetApp.newDataValidation().requireValueInList(Object.keys(PERIODS), true).build(),
  );
  if (!period.getValue()) period.setValue('7 jours');

  const dateRule = SpreadsheetApp.newDataValidation().requireDate().build();
  sheet.getRange(`${CELL_FROM}:${CELL_TO}`).setDataValidation(dateRule).setNumberFormat('dd/MM/yyyy HH:mm');

  const step = sheet.getRange(CELL_STEP);
  step.setDataValidation(
    SpreadsheetApp.newDataValidation().requireValueInList(Object.keys(STEPS), true).build(),
  );
  if (!step.getValue()) step.setValue('Auto');

  sheet.getRange('C4').setValue('← utilisés si Période = Personnalisée').setFontColor('#777777');
  sheet.getRange('C6').setValue('← Auto : au plus 1500 points par courbe').setFontColor('#777777');

  sheet.getRange(SCALES_FIRST_ROW - 2, 1).setValue('Échelles (vide = automatique)').setFontWeight('bold');
  sheet
    .getRange(SCALES_FIRST_ROW - 1, 1, 1, 3)
    .setValues([['Graphique', 'Y min', 'Y max']])
    .setFontWeight('bold');
  const titles = CHARTS.map((chart) => [chart.title]);
  sheet.getRange(SCALES_FIRST_ROW, 1, titles.length, 1).setValues(titles);

  sheet.setColumnWidth(1, 230);
  sheet.setColumnWidth(2, 130);
  sheet.setColumnWidth(3, 110);
  sheet.setFrozenRows(0);

  const view = spreadsheet.getSheetByName(VIEW) || spreadsheet.insertSheet(VIEW);
  view.hideSheet();
}

/** Recalcule l'onglet Vue selon les réglages, puis redessine les graphes. */
function refreshDashboard() {
  const spreadsheet = SpreadsheetApp.getActiveSpreadsheet();
  const dashboard = spreadsheet.getSheetByName(DASHBOARD);
  if (!dashboard) return;
  const view = spreadsheet.getSheetByName(VIEW) || spreadsheet.insertSheet(VIEW);

  const { from, to } = selectedPeriod(dashboard);
  const rows = readMeasures(from, to);
  const stepMinutes = chosenStep(dashboard, rows);
  const table = aggregate(rows, stepMinutes);

  // En-têtes de la vue : horodatage, mesures agrégées, seuils.
  const valueColumns = COLUMNS.filter((c) => c.agg);
  const header = ['Horodatage', ...valueColumns.map((c) => c.title), ...THRESHOLDS.map((t) => t.title)];
  view.clearContents();
  view.getRange(1, 1, 1, header.length).setValues([header]);
  if (table.length > 0) {
    const values = table.map((row) => [...row, ...THRESHOLDS.map((t) => t.value)]);
    view.getRange(2, 1, values.length, header.length).setValues(values);
    view.getRange(2, 1, values.length, 1).setNumberFormat('dd/MM/yyyy HH:mm');
  }

  drawCharts(dashboard, view, header, Math.max(table.length, 1) + 1, stepMinutes);
}

function selectedPeriod(dashboard) {
  const choice = dashboard.getRange(CELL_PERIOD).getValue() || '7 jours';
  const now = new Date();
  if (choice === 'Personnalisée') {
    const from = dashboard.getRange(CELL_FROM).getValue();
    const to = dashboard.getRange(CELL_TO).getValue();
    return {
      from: from instanceof Date ? from : new Date(0),
      to: to instanceof Date ? to : now,
    };
  }
  const days = PERIODS[choice];
  return { from: days ? new Date(now.getTime() - days * 86400000) : new Date(0), to: now };
}

/** Lignes de Mesures entre deux dates. Les lignes sont chronologiques : recherche dichotomique. */
function readMeasures(from, to) {
  const sheet = measuresSheet();
  const count = sheet.getLastRow() - 1;
  if (count <= 0) return [];
  const times = sheet.getRange(2, 1, count, 1).getValues().map((r) => new Date(r[0]).getTime());
  const first = lowerBound(times, from.getTime());
  const last = lowerBound(times, to.getTime() + 1) - 1;
  if (last < first) return [];
  return sheet.getRange(2 + first, 1, last - first + 1, COLUMNS.length).getValues();
}

function lowerBound(sorted, value) {
  let low = 0;
  let high = sorted.length;
  while (low < high) {
    const middle = (low + high) >> 1;
    if (sorted[middle] < value) low = middle + 1;
    else high = middle;
  }
  return low;
}

function chosenStep(dashboard, rows) {
  const choice = dashboard.getRange(CELL_STEP).getValue() || 'Auto';
  const step = STEPS[choice];
  if (step) return step;
  if (rows.length < 2) return 5;
  const spanMinutes = (new Date(rows[rows.length - 1][0]) - new Date(rows[0][0])) / 60000;
  return [5, 15, 60, 1440].find((candidate) => spanMinutes / candidate <= MAX_POINTS) || 1440;
}

/** Début du pas contenant `date`, en heure locale du projet (Europe/Paris). */
function bucketStart(date, stepMinutes) {
  const d = new Date(date);
  if (stepMinutes >= 1440) return new Date(d.getFullYear(), d.getMonth(), d.getDate());
  const minutes = d.getHours() * 60 + d.getMinutes();
  const start = Math.floor(minutes / stepMinutes) * stepMinutes;
  return new Date(d.getFullYear(), d.getMonth(), d.getDate(), 0, start);
}

/** Regroupe les lignes par pas : moyenne, maximum ou somme selon la colonne. */
function aggregate(rows, stepMinutes) {
  const indexed = COLUMNS.map((column, index) => ({ ...column, index })).filter((c) => c.agg);
  if (stepMinutes <= 5) {
    return rows.map((row) => [new Date(row[0]), ...indexed.map((c) => row[c.index])]);
  }

  const buckets = new Map();
  rows.forEach((row) => {
    const start = bucketStart(row[0], stepMinutes).getTime();
    if (!buckets.has(start)) buckets.set(start, indexed.map(() => []));
    const lists = buckets.get(start);
    indexed.forEach((column, i) => {
      const value = row[column.index];
      if (value !== '' && value !== null && !isNaN(value)) lists[i].push(Number(value));
    });
  });

  return [...buckets.entries()]
    .sort((a, b) => a[0] - b[0])
    .map(([start, lists]) => [
      new Date(start),
      ...lists.map((values, i) => {
        if (values.length === 0) return '';
        if (indexed[i].agg === 'max') return Math.max(...values);
        const sum = values.reduce((total, v) => total + v, 0);
        return indexed[i].agg === 'sum' ? sum : Math.round((sum / values.length) * 10) / 10;
      }),
    ]);
}

/** Un graphe par définition de CHARTS, deux par rangée, échelles lues dans le tableau. */
function drawCharts(dashboard, view, header, lastRow, stepMinutes) {
  dashboard.getCharts().forEach((chart) => dashboard.removeChart(chart));
  const scales = dashboard.getRange(SCALES_FIRST_ROW, 2, CHARTS.length, 2).getValues();
  const dateFormat = stepMinutes >= 1440 ? 'dd/MM' : 'dd/MM HH:mm';

  CHARTS.forEach((definition, index) => {
    let builder = dashboard
      .newChart()
      .setChartType(Charts.ChartType.LINE)
      .addRange(view.getRange(1, 1, lastRow, 1));
    const thresholdSeries = {};
    definition.series.forEach((title, seriesIndex) => {
      const column = header.indexOf(title) + 1;
      builder = builder.addRange(view.getRange(1, column, lastRow, 1));
      if (THRESHOLDS.some((t) => t.title === title)) {
        thresholdSeries[seriesIndex] = { color: '#9e9e9e', lineWidth: 1 };
      }
    });

    const [yMin, yMax] = scales[index];
    const viewWindow = {};
    if (yMin !== '' && !isNaN(yMin)) viewWindow.min = Number(yMin);
    if (yMax !== '' && !isNaN(yMax)) viewWindow.max = Number(yMax);

    builder = builder
      .setNumHeaders(1)
      .setOption('title', definition.title)
      .setOption('legend', { position: 'bottom' })
      .setOption('hAxis', { format: dateFormat })
      .setOption('vAxis', { viewWindow })
      .setOption('series', thresholdSeries)
      .setOption('interpolateNulls', true)
      .setOption('width', 620)
      .setOption('height', 300)
      // Graphes à partir de la colonne E, deux par rangée, 16 lignes par graphe.
      .setPosition(1 + Math.floor(index / 2) * 16, 5 + (index % 2) * 7, 0, 0);
    dashboard.insertChart(builder.build());
  });
}

/** Test sans l'écran : ajoute une ligne factice (à supprimer ensuite). */
function testerAjout() {
  const response = doPost({
    postData: {
      contents: JSON.stringify({
        token: TOKEN, co2: 800, cov: 100, t_int: 21.5, h_int: 50, t_ext: 12.3, h_ext: 80,
        pression: 1013.2, pm25: 10, pm10: 18, pm10_max: 30, tendance: -0.8,
        verdict: 'TEST', chantier: false, sonde: true, rx_espnow: 1, rx_wifi: 1, pm1: 6,
      }),
    },
  });
  Logger.log(response.getContent());
}

/**
 * À lancer une fois après un changement d'ordre dans COLUMNS : déplace les
 * colonnes existantes de l'onglet Mesures, avec leurs données, pour qu'elles
 * suivent le nouvel ordre. Les colonnes sont retrouvées par leur titre.
 */
function reorganiserColonnes() {
  const sheet = measuresSheet();
  COLUMNS.forEach((column, index) => {
    const titles = sheet.getRange(1, 1, 1, sheet.getLastColumn()).getValues()[0];
    const current = titles.indexOf(column.title);
    if (current === -1 || current === index) return;
    sheet.moveColumns(sheet.getRange(1, current + 1), index + 1);
  });
  Logger.log(sheet.getRange(1, 1, 1, sheet.getLastColumn()).getValues()[0].join(' | '));
}
